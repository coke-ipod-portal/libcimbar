/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once

/*
 * ChunkedEncoderPlus.h
 *
 * Static helpers for splitting arbitrarily large files into 18 MB chunks
 * that fit within the 25-bit FountainMetadata::file_size ceiling (~32 MB
 * hard max).  The actual per-chunk encoding is performed by cfc_send_chunked
 * via the cimbar_js C API (cimbare_init_encode / cimbare_encode /
 * cimbare_next_frame / cimbare_render) -- the same pipeline used by the
 * upstream cimbar_send binary.
 *
 * Design constraints respected:
 *   - No changes to FountainMetadata wire format.
 *   - No changes to fountain_encoder_stream or any existing encoder.
 *   - No changes to the CFC Android app.
 *   - Each chunk uses a unique encode_id (chunk_idx & 0x7F) so the Android
 *     decoder correctly tracks up to 128 independent chunks in its _done map
 *     without slot collisions.  With 18 MB chunks a 2 GB file needs ~114
 *     chunks, so wrapping never occurs for any realistic transfer.
 */

#include "cimb_translator/Config.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace cimbar
{
	// 18 MB per chunk: safely under the 25-bit (~32 MB) FountainMetadata ceiling
	// even after zstd header + fountain padding overhead.
	static constexpr size_t CHUNK_BYTES = 18ULL * 1024ULL * 1024ULL;

	struct ChunkedEncoderPlus
	{
		// ------------------------------------------------------------------
		// chunk_count
		//   Returns the number of 18 MB chunks required for the given file.
		//   Returns 0 on error (file not found / unreadable / empty).
		// ------------------------------------------------------------------
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
			return (unsigned)((sz + CHUNK_BYTES - 1) / CHUNK_BYTES);
		}

		// ------------------------------------------------------------------
		// chunk_offset
		//   Returns the byte offset into the file where chunk[idx] begins.
		// ------------------------------------------------------------------
		static size_t chunk_offset(unsigned idx)
		{
			return (size_t)idx * CHUNK_BYTES;
		}

		// ------------------------------------------------------------------
		// chunk_size_for
		//   Returns the byte length of chunk[idx] given a total file size.
		//   Returns 0 if idx is out of range.
		// ------------------------------------------------------------------
		static size_t chunk_size_for(unsigned idx, size_t total_file_size)
		{
			size_t off = chunk_offset(idx);
			if (off >= total_file_size)
				return 0;
			return std::min(CHUNK_BYTES, total_file_size - off);
		}
	};

} // namespace cimbar
