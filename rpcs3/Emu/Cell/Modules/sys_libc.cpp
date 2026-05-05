#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_spu.h"

LOG_CHANNEL(sys_libc);
LOG_CHANNEL(sys_libc_spu_printf);

vm::ptr<void> sys_libc_memcpy(vm::ptr<void> dst, vm::cptr<void> src, u32 size)
{
	sys_libc.trace("memcpy(dst=*0x%x, src=*0x%x, size=0x%x)", dst, src, size);

	::memcpy(dst.get_ptr(), src.get_ptr(), size);
	return dst;
}
vm::ptr<void> sys_libc_memset(vm::ptr<void> dst, s32 value, u32 size)
{
	sys_libc.trace("memset(dst=*0x%x, value=0x%x, size=0x%x)", dst, value, size);

	::memset(dst.get_ptr(), value, size);
	return dst;
}

vm::ptr<void> sys_libc_memmove(vm::ptr<void> dst, vm::ptr<void> src, u32 size)
{
	sys_libc.trace("memmove(dst=*0x%x, src=*0x%x, size=0x%x)", dst, src, size);

	::memmove(dst.get_ptr(), src.get_ptr(), size);
	return dst;
}

u32 sys_libc_memcmp(vm::ptr<void> buf1, vm::ptr<void> buf2, u32 size)
{
	sys_libc.trace("memcmp(buf1=*0x%x, buf2=*0x%x, size=0x%x)", buf1, buf2, size);

	return ::memcmp(buf1.get_ptr(), buf2.get_ptr(), size);
}

// --- spu_thread_printf HLE ---

// %n intentionally excluded — guest-controlled snprintf %n is a host memory write primitive
static bool is_spu_printf_conversion(char c)
{
	switch (c)
	{
	case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
	case 'c': case 's': case 'p':
	case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
	case 'a': case 'A':
	case '%':
		return true;
	default:
		return false;
	}
}

static void append_str(char *buf, size_t cap, size_t *len, const char *src)
{
	size_t avail = cap - *len - 1;
	size_t n = std::strlen(src);
	if (n > avail) n = avail;
	std::memcpy(buf + *len, src, n);
	*len += n;
	buf[*len] = '\0';
}

static unsigned read_spu_ls_string(ppu_thread& ppu, u32 spu_thread_id, u32 lsa, char *buf, size_t cap, vm::var<u64>& value)
{
	unsigned i = 0;
	if (cap == 0) return 0;
	for (; i < cap - 1; i++)
	{
		if (sys_spu_thread_read_ls(ppu, spu_thread_id, lsa + i, value, 1) != CELL_OK)
			break;
		u8 b = static_cast<u8>(static_cast<u32>(*value));
		if (b == 0) break;
		buf[i] = static_cast<char>(b);
	}
	buf[i] = '\0';
	return i;
}

static constexpr u32 SPU_PRINTF_OUT_BUFCAP   = 1024;
static constexpr u32 SPU_PRINTF_SPEC_BUFCAP  = 64;
static constexpr u32 SPU_PRINTF_S_BUFCAP     = 256;
static constexpr int SPU_PRINTF_MAX_VARARGS  = 15;

// Returns byte count written to out_buf (excluding NUL), or -1 on error.
static int format_spu_printf(ppu_thread& ppu, u32 spu_thread_id, u32 arg_addr, char *out_buf, vm::var<u64>& value)
{
	out_buf[0] = '\0';
	size_t out_len = 0;

	// fmt pointer is in bytes 0..3 of the first slot (caller's $4)
	if (sys_spu_thread_read_ls(ppu, spu_thread_id, arg_addr, value, 4) != CELL_OK)
		return -1;

	u32 fmt_addr = static_cast<u32>(*value);
	if (fmt_addr == 0) return -1;

	// Read format string into local buffer up front
	char fmt[SPU_PRINTF_OUT_BUFCAP];
	read_spu_ls_string(ppu, spu_thread_id, fmt_addr, fmt, sizeof(fmt), value);

	int slot = 1; // next vararg lives at slot 1 (offset 16); slot 0 was fmt

	for (const char *p = fmt; *p; )
	{
		if (*p != '%')
		{
			char one[2] = { *p++, '\0' };
			append_str(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, one);
			continue;
		}

		// '%' — collect spec up to and including the conversion char
		char spec[SPU_PRINTF_SPEC_BUFCAP];
		size_t sp = 0;
		spec[sp++] = '%';
		p++;
		while (*p && sp < sizeof(spec) - 1 && !is_spu_printf_conversion(*p))
		{
			spec[sp++] = *p++;
		}
		if (!*p) break; // truncated spec at end of format
		char conv = *p++;
		spec[sp++] = conv;
		spec[sp]   = '\0';

		if (conv == '%')
		{
			append_str(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, "%");
			continue;
		}
		if (slot > SPU_PRINTF_MAX_VARARGS)
		{
			// Out of saved-register slots; print spec verbatim and stop
			append_str(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, spec);
			continue;
		}

		// Detect length modifier — distinguish ll (8-byte) from default (4-byte)
		int is_ll = 0;
		for (size_t i = 1; i + 1 < sp; i++)
		{
			if (spec[i] == 'l' && spec[i + 1] == 'l') { is_ll = 1; break; }
		}
		int is_double = (conv == 'f' || conv == 'F' || conv == 'e' ||
		                 conv == 'E' || conv == 'g' || conv == 'G' ||
		                 conv == 'a' || conv == 'A');

		u32 slot_addr = arg_addr + static_cast<u32>(slot * 16);
		char piece[SPU_PRINTF_S_BUFCAP];

		if (conv == 's')
		{
			if (sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 4) != CELL_OK)
			{
				std::snprintf(piece, sizeof(piece), spec, "(spu-ls-read-fail)");
			}
			else
			{
				u32 s_addr = static_cast<u32>(*value);
				if (s_addr == 0)
				{
					std::snprintf(piece, sizeof(piece), spec, "(null)");
				}
				else
				{
					char sbuf[SPU_PRINTF_S_BUFCAP];
					read_spu_ls_string(ppu, spu_thread_id, s_addr, sbuf, sizeof(sbuf), value);
					std::snprintf(piece, sizeof(piece), spec, sbuf);
				}
			}
		}
		else if (conv == 'c')
		{
			sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 4);
			std::snprintf(piece, sizeof(piece), spec, static_cast<int>(static_cast<u32>(*value) & 0xff));
		}
		else if (conv == 'p')
		{
			sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 4);
			std::snprintf(piece, sizeof(piece), "0x%08x", static_cast<unsigned>(static_cast<u32>(*value)));
		}
		else if (is_double)
		{
			sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 8);
			u64 raw = *value;
			double d;
			std::memcpy(&d, &raw, sizeof(d));
			std::snprintf(piece, sizeof(piece), spec, d);
		}
		else if (is_ll)
		{
			sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 8);
			u64 raw64 = *value;
			if (conv == 'd' || conv == 'i')
			{
				std::snprintf(piece, sizeof(piece), spec, static_cast<long long>(raw64));
			}
			else
			{
				std::snprintf(piece, sizeof(piece), spec, static_cast<unsigned long long>(raw64));
			}
		}
		else
		{
			sys_spu_thread_read_ls(ppu, spu_thread_id, slot_addr, value, 4);
			u32 raw32 = static_cast<u32>(*value);
			if (conv == 'd' || conv == 'i')
			{
				std::snprintf(piece, sizeof(piece), spec, static_cast<int>(raw32));
			}
			else
			{
				std::snprintf(piece, sizeof(piece), spec, static_cast<unsigned>(raw32));
			}
		}
		piece[sizeof(piece) - 1] = '\0';
		append_str(out_buf, SPU_PRINTF_OUT_BUFCAP, &out_len, piece);
		slot++;
	}

	return static_cast<int>(out_len);
}

s32 sys_libc_spu_thread_printf(ppu_thread& ppu, u32 spu_thread_id, u32 arg_addr)
{
	// Single vm::var reused for all LS reads in this call to avoid per-byte stack allocations
	vm::var<u64> value;

	char out_buf[SPU_PRINTF_OUT_BUFCAP];
	int len = format_spu_printf(ppu, spu_thread_id, arg_addr, out_buf, value);

	if (len < 0)
	{
		return -1; // printf-style error
	}

	sys_libc_spu_printf.success("%s", out_buf);
	return len; // byte count (printf-style success)
}

DECLARE(ppu_module_manager::sys_libc)("sys_libc", []()
{
	REG_FNID(sys_libc, "memcpy", sys_libc_memcpy)/*.flag(MFF_FORCED_HLE)*/;
	REG_FNID(sys_libc, "memset", sys_libc_memset);
	REG_FNID(sys_libc, "memmove", sys_libc_memmove);
	REG_FNID(sys_libc, "memcmp", sys_libc_memcmp);
	REG_FNID(sys_libc, "spu_thread_printf", sys_libc_spu_thread_printf);
});
