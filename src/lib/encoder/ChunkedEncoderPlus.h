/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once

/*
 * ChunkedEncoderPlus.h
 *
 * Wraps EncoderPlus to transparently split arbitrarily large files into
 * chunks that fit within the 25-bit FountainMetadata::file_size ceiling
 * (~32 MB hard max; we use 18 MB to leave comfortable headroom for
 * zstd-compressed overhead and fountain padding).
 *
 * Each chunk is transmitted as a fully independent, valid cimbar fountain
 * session. The Android CFC app receives and writes each chunk to disk
 * (named "<basename>.partN") without any modification. A reassembly
 * step (`cat *.part0 *.part1 ... > output`) completes the transfer.
 *
 * Design constraints respected:
 *   - No changes to FountainMetadata wire format.
 *   - No changes to fountain_encoder_stream or fountain_decoder_sink.
 *   - No changes to Encoder.h / EncoderPlus.h.
 *   - No changes to the CFC Android app.
 *   - Each chunk uses a unique encode_id derived from chunk index so the
 *     Android decoder correctly tracks up to 128 independent chunks in its
 *     _done map without slot collisions.
 */

#include "EncoderPlus.h"
#include "extractor/Scanner.h"
#include "cimb_translator/Config.h"
#include "serialize/format.h"
#include "util/File.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cimbar
{
	// 18 MB per chunk: safely under the 25-bit (~32 MB) FountainMetadata ceiling
	// even after zstd header + fountain padding overhead.
	static constexpr size_t CHUNK_BYTES = 18ULL * 1024ULL * 1024ULL;

	class ChunkedEncoderPlus : public EncoderPlus
	{
	public:
		using EncoderPlus::EncoderPlus;

		// ---------------------------------------------------------------
		// Static helpers
		// ---------------------------------------------------------------

		// Returns the number of 18 MB chunks required for the given file.
		// Returns 0 on error (file not found / unreadable).
		static unsigned chunk_count(const std::string& filename)
		{
			std::error_code ec;
			uintmax_t sz = std::filesystem::file_size(filename, ec);
			if (ec || sz == 0)
			{
				std::cerr << "[ChunkedEncoderPlus] chunk_count: cannot stat file '"
				          << filename << "': " << ec.message() << "\n";
				return 0;
			}
			return (unsigned)((sz + CHUNK_BYTES - 1) / CHUNK_BYTES);
		}

		// Returns the byte offset into the file where chunk[idx] begins.
		static size_t chunk_offset(unsigned idx)
		{
			return (size_t)idx * CHUNK_BYTES;
		}

		// Returns the expected byte size of chunk[idx] for a given total file size.
		static size_t chunk_size_for(unsigned idx, size_t total_file_size)
		{
			size_t offset = chunk_offset(idx);
			if (offset >= total_file_size)
				return 0;
			return std::min(CHUNK_BYTES, total_file_size - offset);
		}

		// ---------------------------------------------------------------
		// Core encode method
		// ---------------------------------------------------------------

		/**
		 * Encode a single chunk of the source file, calling on_frame() for
		 * every generated cimbar frame.
		 *
		 * @param filename         Source file path.
		 * @param idx              Zero-based chunk index.
		 * @param on_frame         Callback: receives (cv::Mat frame, unsigned frame_number).
		 *                         Return false to abort early (e.g. window closed).
		 * @param compression_level zstd level (0 = none, 16 = default).
		 * @param redundancy       Fountain redundancy multiplier (default 4.0).
		 * @return                 Number of frames rendered, or 0 on error.
		 */
		unsigned encode_chunk(
			const std::string& filename,
			unsigned idx,
			const std::function<bool(const cv::Mat&, unsigned)>& on_frame,
			int compression_level = 16,
			double redundancy = 4.0)
		{
			// --- Validate file ---
			std::error_code ec;
			uintmax_t total_size = std::filesystem::file_size(filename, ec);
			if (ec || total_size == 0)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: cannot stat '"
				          << filename << "': " << ec.message() << "\n";
				return 0;
			}

			size_t offset = chunk_offset(idx);
			if (offset >= total_size)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: chunk idx " << idx
				          << " is out of range (file size " << total_size << ")\n";
				return 0;
			}
			size_t read_size = std::min(CHUNK_BYTES, (size_t)(total_size - offset));

			// --- Read chunk bytes ---
			std::ifstream f(filename, std::ios::binary);
			if (!f)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: failed to open '"
				          << filename << "'\n";
				return 0;
			}
			f.seekg((std::streamoff)offset);
			if (!f)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: seekg failed for '"
				          << filename << "' at offset " << offset << "\n";
				return 0;
			}

			std::string buf(read_size, '\0');
			f.read(buf.data(), (std::streamsize)read_size);
			size_t got = (size_t)f.gcount();
			if (got == 0)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: read 0 bytes at offset "
				          << offset << " from '" << filename << "'\n";
				return 0;
			}
			buf.resize(got);

			// --- Build per-chunk virtual filename for the zstd frame header ---
			// The Android app uses this name when writing the file to disk.
			// Format: "<basename>.partN" so reassembly is trivial.
			std::string base = File::basename(filename);
			std::string chunk_name = fmt::format("{}.part{}", base, idx);

			// --- Assign encode_id: low 7 bits of idx ---
			// This ensures each chunk gets a unique session identity in the
			// Android FountainDecoder _done map (via md.id() = encode_id + size).
			// We cycle through [0,127]. With 18 MB chunks, a 2 GB file needs
			// ~114 chunks, so we never overlap for any realistic transfer.
			set_encode_id((uint8_t)(idx & 0x7F));

			// --- Feed into fountain encoder via stringstream ---
			std::stringstream ss;
			ss.write(buf.data(), (std::streamsize)buf.size());
			if (!ss)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: stringstream write failed\n";
				return 0;
			}

			// create_fountain_encoder compresses + creates the FountainEncoderStream
			auto fes = create_fountain_encoder(ss, chunk_name, compression_level);
			if (!fes)
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: create_fountain_encoder "
				             "returned null for chunk " << idx << "\n";
				return 0;
			}

			if (!fes->good())
			{
				std::cerr << "[ChunkedEncoderPlus] encode_chunk: fountain encoder stream "
				             "is not good() for chunk " << idx << "\n";
				return 0;
			}

			// --- Calculate required frames with redundancy ---
			unsigned chunks_per_frame = cimbar::Config::fountain_chunks_per_frame(_bitsPerSymbol);
			if (chunks_per_frame == 0)
				chunks_per_frame = 1; // defensive: avoid divide-by-zero

			unsigned required_frames = (unsigned)(
				((double)fes->blocks_required() * redundancy) / (double)chunks_per_frame
			);
			if (required_frames == 0)
				required_frames = 1;

			// --- Encode frames ---
			unsigned frames_rendered = 0;
			unsigned consecutive_scan_failures = 0;
			static constexpr unsigned MAX_CONSECUTIVE_SCAN_FAIL = 5;

			while (frames_rendered < required_frames)
			{
				auto frame = encode_next(*fes);
				if (!frame)
				{
					std::cerr << "[ChunkedEncoderPlus] encode_chunk: encode_next() "
					             "returned nullopt at frame " << frames_rendered
					          << " (chunk " << idx << ")\n";
					break;
				}

				// Skip frames that would confuse the scanner (known edge case in 8x8 mode)
				if (!Scanner::will_it_scan(*frame))
				{
					++consecutive_scan_failures;
					if (consecutive_scan_failures < MAX_CONSECUTIVE_SCAN_FAIL)
						continue;
					// After 5 consecutive failures, emit a warning and force forward progress
					std::cerr << "[ChunkedEncoderPlus] WARNING: " << consecutive_scan_failures
					          << " consecutive unscannable frames (chunk " << idx
					          << "). Possible bug. Forcing forward progress.\n";
				}
				consecutive_scan_failures = 0;

				if (!on_frame(*frame, frames_rendered))
					break; // caller signalled stop (e.g. window closed)

				++frames_rendered;
			}

			return frames_rendered;
		}
	}; // class ChunkedEncoderPlus

} // namespace cimbar
