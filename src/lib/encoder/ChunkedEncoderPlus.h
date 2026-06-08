/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once

/*
 * ChunkedEncoderPlus.h
 * ====================
 * Pure static helper math for splitting arbitrarily large files into fixed-
 * size 18 MB chunks before each chunk is sent as an independent cimbar
 * fountain session.
 *
 * WHY CHUNKING?
 *   FountainMetadata::file_size is a 25-bit field, giving a hard ceiling of
 *   ~32 MB per fountain session.  cimbar_send already handles files up to
 *   that limit natively.  This header provides the arithmetic needed to send
 *   files of any size by splitting them, with each chunk staying well under
 *   the ceiling even after zstd + fountain overhead.
 *
 * HOW IT FITS INTO THE SYSTEM:
 *   Actual per-chunk encoding is performed by cfc_send_chunked via the
 *   cimbar_js C API:
 *
 *     cimbare_init_encode(chunk_name, name_len, encode_id)
 *     cimbare_encode(data, size)          // feed raw chunk bytes
 *     while (running) {
 *         cimbare_next_frame();           // generate next fountain frame
 *         cimbare_render();              // display in GLFW window
 *     }
 *
 *   This is the same pipeline used by the upstream cimbar_send binary.
 *
 * ANDROID COMPATIBILITY (no Android changes required):
 *   Each chunk uses encode_id = (chunk_idx & 0x7F) so the CFC Android app
 *   correctly tracks up to 128 independent chunks in its fountain decoder
 *   _done map without slot collisions.  At 18 MB/chunk a 2 GB file needs
 *   ~114 chunks, so id wrapping never occurs for any realistic transfer.
 *
 *   The Android app writes each received session to a file named after the
 *   zstd-frame filename field.  cfc_send_chunked sets that field to
 *   "<basename>.partN", so parts land as separate files and are trivially
 *   reassembled with `cat` or scripts/reassemble.py.
 *
 * DESIGN CONSTRAINTS (unchanged from upstream):
 *   - No changes to FountainMetadata wire format.
 *   - No changes to fountain_encoder_stream or fountain_decoder_sink.
 *   - No changes to Encoder.h / EncoderPlus.h.
 *   - No changes to the CFC Android app.
 */

#include "cimb_translator/Config.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace cimbar
{
	/*
	 * CHUNK_BYTES — maximum bytes per chunk (18 MB).
	 *
	 * Chosen to be safely below the 25-bit FountainMetadata::file_size
	 * ceiling (~33.5 MB) after accounting for:
	 *   - zstd compressed size growth on incompressible data (~0.1%)
	 *   - fountain encoder block header overhead
	 *   - any future FountainMetadata reserved-bit usage
	 *
	 * Increasing this value without checking FountainMetadata will break
	 * the Android decoder silently (file_size truncates, decode fails).
	 */
	static constexpr size_t CHUNK_BYTES = 18ULL * 1024ULL * 1024ULL;

	/*
	 * ChunkedEncoderPlus
	 * ------------------
	 * Stateless struct: all methods are static.  No instantiation required.
	 * Used by cfc_send_chunked.cpp to compute file split geometry.
	 */
	struct ChunkedEncoderPlus
	{
		/*
		 * chunk_count(filename)
		 * Returns the number of CHUNK_BYTES-sized chunks required to cover
		 * the entire file.  Last chunk may be smaller than CHUNK_BYTES.
		 * Returns 0 on error (file not found, unreadable, or empty) and
		 * prints a diagnostic to stderr.
		 */
		static unsigned chunk_count(const std::string& filename)
		{
			std::error_code ec;
			uintmax_t sz = std::filesystem::file_size(filename, ec);
			if (ec || sz == 0)
			{
				std::cerr << "[ChunkedEncoderPlus] chunk_count: cannot stat '"
				          << filename << "': " << ec.message() << "\n";
				return 0;
			}
			// Ceiling division: (sz + CHUNK_BYTES - 1) / CHUNK_BYTES
			return (unsigned)((sz + CHUNK_BYTES - 1) / CHUNK_BYTES);
		}

		/*
		 * chunk_offset(idx)
		 * Returns the byte offset within the source file at which chunk[idx]
		 * begins.  Multiply by CHUNK_BYTES: chunk 0 starts at 0, chunk 1 at
		 * 18 MB, chunk 2 at 36 MB, and so on.
		 */
		static size_t chunk_offset(unsigned idx)
		{
			return (size_t)idx * CHUNK_BYTES;
		}

		/*
		 * chunk_size_for(idx, total_file_size)
		 * Returns the number of bytes in chunk[idx] given the total file
		 * size.  All chunks except the last are exactly CHUNK_BYTES.  The
		 * last chunk is (total_file_size % CHUNK_BYTES), or CHUNK_BYTES if
		 * the file is an exact multiple.  Returns 0 if idx is out of range.
		 */
		static size_t chunk_size_for(unsigned idx, size_t total_file_size)
		{
			size_t off = chunk_offset(idx);
			if (off >= total_file_size)
				return 0; // idx beyond end of file
			return std::min(CHUNK_BYTES, total_file_size - off);
		}
	};

} // namespace cimbar
