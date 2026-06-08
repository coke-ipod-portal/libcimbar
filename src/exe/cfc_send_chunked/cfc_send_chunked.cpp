/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */

/*
 * cfc_send_chunked -- Large-file sender for the CFC / cimbar ecosystem
 * =====================================================================
 * Sends arbitrarily large files (2 GB+) to the unmodified CFC Android app
 * by splitting them into 18 MB chunks and transmitting each chunk as an
 * independent cimbar fountain session.
 *
 * Render path: uses the same cimbar_js C API as cimbar_send:
 *   cimbare_init_encode() -> cimbare_encode() -> [cimbare_next_frame() +
 *   cimbare_render()] loop until encode_id wraps (user confirmed), then
 *   advance to next chunk.
 *
 * Usage:
 *   cfc_send_chunked [options] <file> [<file2> ...]
 *
 * Options:
 *   -f / --fps         Target frames per second  (default: 15)
 *   -m / --mode        Cimbar mode: B, Bm, Bu, 4C  (default: B)
 *   -p / --padding     Black border padding in pixels  (default: 32)
 *   -z / --compression zstd compression level 0-22   (default: 16)
 *   -s / --start-chunk First chunk index to send (default: 0, for resume)
 *   -e / --end-chunk   Last chunk index to send inclusive (default: last)
 *   -h / --help        Print usage
 *
 * Resume support:
 *   If a transfer is interrupted, rerun with --start-chunk N to skip
 *   already-received chunks.
 */

#include "encoder/ChunkedEncoderPlus.h"
#include "cimbar_js/cimbar_js.h"
#include "cimb_translator/Config.h"
#include "serialize/str.h"
#include "util/File.h"
#include "serialize/format.h"

#include "cxxopts/cxxopts.hpp"
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using std::string;
using std::vector;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
namespace {
	volatile std::sig_atomic_t g_stop = 0;
	void handle_signal(int) { g_stop = 1; }

	template <typename TP>
	TP wait_for_frame_time(unsigned delay_ms, const TP& start)
	{
		unsigned elapsed = (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::high_resolution_clock::now() - start).count();
		if (delay_ms > elapsed)
			std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms - elapsed));
		return std::chrono::high_resolution_clock::now();
	}

	void print_progress(unsigned chunk_idx, unsigned total_chunks,
	                    int frame_count, const string& basename)
	{
		fprintf(stderr,
			"\r[cfc_send_chunked] Chunk %u/%u  |  %s.part%u  |  frame %d   ",
			chunk_idx + 1, total_chunks,
			basename.c_str(), chunk_idx,
			frame_count);
		fflush(stderr);
	}
} // namespace

// ---------------------------------------------------------------------------
// send_chunk: encode one 18 MB chunk through the cimbar_js pipeline.
//
// Strategy (mirrors cimbar_send exactly):
//   1. cimbare_init_encode(chunk_name, encode_id=chunk_idx & 0x7F)
//   2. Feed chunk bytes via a single cimbare_encode() call (<=18 MB).
//   3. Drive cimbare_next_frame() + cimbare_render() until:
//        a. The user presses Enter (stdin thread signals enter_pressed), OR
//        b. g_stop is set (SIGINT/SIGTERM), OR
//        c. cimbare_render() returns < 0 (window closed).
//      We keep looping so the chunk displays indefinitely until confirmed --
//      matching the cimbar_send behaviour (fountain restarts automatically
//      when block_count exceeds required inside next_frame).
// ---------------------------------------------------------------------------
static int send_chunk(
	const string& filename,
	unsigned chunk_idx,
	unsigned total_chunks,
	unsigned delay_ms,
	const string& basename)
{
	// --- Read chunk bytes ---
	std::error_code ec;
	uintmax_t total_size = std::filesystem::file_size(filename, ec);
	if (ec || total_size == 0)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: cannot stat '"
		          << filename << "': " << ec.message() << "\n";
		return -1;
	}

	size_t offset   = cimbar::ChunkedEncoderPlus::chunk_offset(chunk_idx);
	size_t raw_size = cimbar::ChunkedEncoderPlus::chunk_size_for(chunk_idx, (size_t)total_size);
	if (raw_size == 0)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: chunk " << chunk_idx
		          << " is out of range (file size " << total_size << ")\n";
		return -1;
	}

	std::ifstream f(filename, std::ios::binary);
	if (!f)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: cannot open '" << filename << "'\n";
		return -1;
	}
	f.seekg((std::streamoff)offset);
	if (!f)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: seekg failed at offset "
		          << offset << " in '" << filename << "'\n";
		return -1;
	}

	string buf(raw_size, '\0');
	f.read(buf.data(), (std::streamsize)raw_size);
	size_t got = (size_t)f.gcount();
	if (got == 0)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: read 0 bytes at offset "
		          << offset << " from '" << filename << "'\n";
		return -1;
	}
	buf.resize(got);

	// Virtual filename for the zstd frame header: Android writes "<basename>.partN"
	string chunk_name = fmt::format("{}.part{}", basename, chunk_idx);

	// encode_id: unique per chunk so Android decoder tracks them separately
	int encode_id = (int)(chunk_idx & 0x7F);

	// --- cimbar_js pipeline: init ---
	if (cimbare_init_encode(chunk_name.c_str(), (unsigned)chunk_name.size(), encode_id) < 0)
	{
		std::cerr << "\n[cfc_send_chunked] ERROR: cimbare_init_encode failed for chunk "
		          << chunk_idx << "\n";
		return -1;
	}

	// --- Feed chunk bytes (single call, mirroring cimbar_send send.cpp:120-130) ---
	{
		const unsigned char* data = reinterpret_cast<const unsigned char*>(buf.data());
		int res = cimbare_encode(data, (unsigned)buf.size());
		if (res < 0)
		{
			std::cerr << "\n[cfc_send_chunked] ERROR: cimbare_encode returned "
			          << res << " for chunk " << chunk_idx << "\n";
			return -1;
		}
		// res == 1: buf.size() == bufsize exactly; send null finaliser (cimbar_send pattern)
		if (res == 1 && cimbare_encode(nullptr, 0) != 0)
		{
			std::cerr << "\n[cfc_send_chunked] ERROR: cimbare_encode finalise failed for chunk "
			          << chunk_idx << "\n";
			return -1;
		}
	}

	std::cerr << "\n[cfc_send_chunked] --- Chunk " << (chunk_idx + 1)
	          << " / " << total_chunks << "  [" << chunk_name << "] ---\n"
	          << "  Point your phone at the screen.\n"
	          << "  Press Enter when fully received (Ctrl-C to abort).\n";

	// --- Detached stdin thread: signals when Enter is pressed ---
	std::atomic<bool> enter_pressed{false};
	{
		std::thread t([&enter_pressed]() {
			char c;
			if (std::cin.get(c))
				enter_pressed.store(true, std::memory_order_release);
		});
		t.detach();
	}

	// --- Render loop: cimbare_next_frame() + cimbare_render() ---
	auto frame_start = std::chrono::high_resolution_clock::now();
	int  frameCount  = 0;
	while (!g_stop && !enter_pressed.load(std::memory_order_acquire))
	{
		frame_start = wait_for_frame_time(delay_ms, frame_start);

		int fc = cimbare_next_frame();
		if (fc < 0)
		{
			std::cerr << "\n[cfc_send_chunked] ERROR: cimbare_next_frame returned "
			          << fc << " (no encoder stream). Aborting chunk.\n";
			return -1;
		}
		frameCount = fc;
		print_progress(chunk_idx, total_chunks, frameCount, basename);

		if (cimbare_render() < 0)
		{
			std::cerr << "\n[cfc_send_chunked] Window closed -- stopping.\n";
			g_stop = 1;
			return -2;
		}
	}

	fprintf(stderr, "\n");
	return frameCount;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
	std::signal(SIGINT,  handle_signal);
	std::signal(SIGTERM, handle_signal);

	cxxopts::Options options("cfc_send_chunked",
		"Send large files (2 GB+) to the CFC Android app via chunked cimbar encoding.");

	unsigned defaultFps     = 15;
	unsigned defaultPadding = 32;
	int      defaultComp    = (int)cimbar::Config::compression_level();

	options.add_options()
		("i,in",         "Source file(s)",
		                  cxxopts::value<vector<string>>())
		("f,fps",         "Target FPS",
		                  cxxopts::value<unsigned>()->default_value(turbo::str::str(defaultFps)))
		("m,mode",        "Cimbar mode [B, Bm, Bu, 4C]",
		                  cxxopts::value<string>()->default_value("B"))
		("p,padding",     "Border padding in pixels",
		                  cxxopts::value<unsigned>()->default_value(turbo::str::str(defaultPadding)))
		("z,compression", "zstd compression level (0=none)",
		                  cxxopts::value<int>()->default_value(turbo::str::str(defaultComp)))
		("s,start-chunk", "First chunk index to send (0-based, for resume)",
		                  cxxopts::value<unsigned>()->default_value("0"))
		("e,end-chunk",   "Last chunk index to send inclusive (default: last)",
		                  cxxopts::value<int>()->default_value("-1"))
		("h,help",        "Print usage")
	;
	options.show_positional_help();
	options.parse_positional({"in"});
	options.positional_help("<file> [<file2> ...]");

	cxxopts::ParseResult result;
	try {
		result = options.parse(argc, argv);
	} catch (const cxxopts::exceptions::exception& e) {
		std::cerr << "[cfc_send_chunked] argument error: " << e.what() << "\n"
		          << options.help() << "\n";
		return 1;
	}

	if (result.count("help") || !result.count("in"))
	{
		std::cout << options.help() << "\n";
		return 0;
	}

	vector<string> infiles   = result["in"].as<vector<string>>();
	int   compression        = result["compression"].as<int>();
	unsigned fps             = result["fps"].as<unsigned>();
	unsigned padding         = result["padding"].as<unsigned>();
	unsigned start_chunk     = result["start-chunk"].as<unsigned>();
	int      end_chunk_arg   = result["end-chunk"].as<int>();

	if (fps == 0)     fps     = defaultFps;
	if (padding == 0) padding = defaultPadding;
	unsigned delay_ms = (fps > 0) ? (1000u / fps) : 66u;

	// --- Cimbar mode ---
	unsigned config_mode = 68; // default: B
	if (result.count("mode"))
	{
		string mode = result["mode"].as<string>();
		if      (mode == "4c" || mode == "4C") config_mode = 4;
		else if (mode == "Bu" || mode == "BU") config_mode = 66;
		else if (mode == "Bm" || mode == "BM") config_mode = 67;
		// "B" or anything else => 68
	}
	cimbar::Config::update(config_mode);

	// --- Validate input files ---
	for (const auto& fn : infiles)
	{
		if (!std::filesystem::exists(fn))
		{
			std::cerr << "[cfc_send_chunked] ERROR: file not found: '" << fn << "'\n";
			return 2;
		}
	}

	// --- Init GLFW window via cimbar_js C API ---
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
	int win_w = (int)cimbar::Config::image_size_x();
	int win_h = (int)cimbar::Config::image_size_y();
	if (cimbare_init_window(win_w, win_h) < 0)
	{
		std::cerr << "[cfc_send_chunked] ERROR: failed to create GLFW window.\n";
		return 70;
	}
	cimbare_auto_scale_window(padding);
	cimbare_configure(config_mode, compression);

	std::cerr << "[cfc_send_chunked] Window ready.  mode=" << config_mode
	          << "  fps=" << fps << "  padding=" << padding
	          << "  compression=" << compression << "\n";

	// --- Process each input file ---
	for (const auto& filename : infiles)
	{
		if (g_stop) break;

		unsigned total_chunks = cimbar::ChunkedEncoderPlus::chunk_count(filename);
		if (total_chunks == 0)
		{
			std::cerr << "\n[cfc_send_chunked] ERROR: could not determine chunk count for '"
			          << filename << "' (empty or unreadable). Skipping.\n";
			continue;
		}

		unsigned end_chunk = (end_chunk_arg < 0 || (unsigned)end_chunk_arg >= total_chunks)
			? total_chunks - 1
			: (unsigned)end_chunk_arg;

		if (start_chunk > end_chunk)
		{
			std::cerr << "\n[cfc_send_chunked] ERROR: --start-chunk (" << start_chunk
			          << ") > --end-chunk (" << end_chunk << ") for '" << filename << "'. Skipping.\n";
			continue;
		}

		std::error_code size_ec;
		uintmax_t file_bytes = std::filesystem::file_size(filename, size_ec);
		string base = File::basename(filename);

		std::cerr << "\n[cfc_send_chunked] File: '" << filename << "'\n"
		          << "  Size:   " << file_bytes << " bytes ("
		          << (file_bytes / (1024*1024)) << " MB)\n"
		          << "  Chunks: " << total_chunks
		          << "  (sending idx " << start_chunk << ".." << end_chunk << ")\n"
		          << "  Chunk size: " << (cimbar::CHUNK_BYTES / (1024*1024)) << " MB each\n";

		// --- Per-chunk loop ---
		for (unsigned idx = start_chunk; idx <= end_chunk && !g_stop; ++idx)
		{
			int rc = send_chunk(filename, idx, total_chunks, delay_ms, base);
			if (rc == -2)
				break; // window closed
			if (rc < 0)
			{
				std::cerr << "[cfc_send_chunked] Chunk " << idx
				          << " failed (rc=" << rc << "). Aborting file.\n";
				break;
			}
			std::cerr << "[cfc_send_chunked] Chunk " << (idx+1) << " / "
			          << total_chunks << " confirmed.  frames_rendered=" << rc << "\n";
		}

		// --- Reassembly instructions ---
		if (!g_stop)
		{
			std::cout << "\n========================================\n"
			          << "All " << total_chunks << " chunk(s) sent for: " << filename << "\n"
			          << "\nReassemble on Android (Termux, in the CFC save directory):\n"
			          << "  cat";
			for (unsigned i = 0; i < total_chunks; ++i)
				std::cout << " '" << base << ".part" << i << "'";
			std::cout << " > '" << base << "'\n"
			          << "  # or: python3 scripts/reassemble.py '" << filename << "'\n"
			          << "========================================\n";
		}
		else
		{
			std::cerr << "\n[cfc_send_chunked] Interrupted. Resume with:\n"
			          << "  cfc_send_chunked --start-chunk <N> '" << filename << "'\n";
		}
	}

	return g_stop ? 1 : 0;
}
