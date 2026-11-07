/* SPDX-License-Identifier: GPL-2.0-or-later (with xemu's DSP it hosts; see NOTICE.md) */
/* Blood Wake: xemu's trace events, compiled out. */
#ifndef BW_XEMU_SHIM_TRACE_H
#define BW_XEMU_SHIM_TRACE_H
#define trace_dsp_read_peripheral(...) ((void)0)
#define trace_dsp_write_peripheral(...) ((void)0)
#define trace_event_get_state(ev) 0
#define trace_dsp56k_execute_instruction(...) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(...) ((void)0)
#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 0
#endif
