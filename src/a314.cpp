/*
 * A314 Clockport Hardware Emulation
 *
 * This file emulates the A314-CP hardware interface that sits on the
 * Amiga's clockport and provides communication with a virtual Raspberry Pi.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "a314.h"
#include "debug.h"

#include <string.h>

#define write_log_verbose(...) do { } while(0)

#define CLOCKPORT_ADDR          0xd80001
#define SHMEM_SIZE              0x10000

#define REG_SRAM                0
#define REG_IRQ                 1
#define REG_ADDR_LO             2
#define REG_ADDR_HI             3

#define REG_IRQ_SET             0x80
#define REG_IRQ_CLR             0x00
#define REG_IRQ_PI              0x02
#define REG_IRQ_CP              0x01

// Offset relative to communication area for queue pointers.
#define R2A_TAIL_OFFSET         0
#define A2R_HEAD_OFFSET         1
#define A2R_TAIL_OFFSET         2
#define R2A_HEAD_OFFSET         3
#define RESTART_COUNTER         4
#define MAGIC_BYTE1             5
#define MAGIC_BYTE2             6

// Addresses of fixed data structures in shared memory.
#define A2R_BASE                0
#define R2A_BASE                256
#define CAP_BASE                512

// Hardware state
static struct
{
	bool enabled;
	uae_u16 address_ptr;
	uae_u8 irq_status;
	uae_u8 shared_memory[SHMEM_SIZE];
	uae_u8 restart_counter;
	bool amiga_irq_pending;
} a314_state;

void a314d_emu_init(uae_u8 *shmem, int shmem_size);
void a314d_emu_cleanup(void);
void a314d_emu_reset(void);
void a314d_emu_set_irq(void);

extern "C" {

void a314_init(void)
{
	if (!currprefs.a314_emulation)
	{
		a314_state.enabled = false;
		return;
	}

	write_log("A314: Initializing A314-CP emulation\n");

	memset(&a314_state, 0, sizeof(a314_state));
	a314_state.enabled = true;
	a314_state.restart_counter = 0;

	a314_state.shared_memory[CAP_BASE + MAGIC_BYTE1] = 0xa3;
	a314_state.shared_memory[CAP_BASE + MAGIC_BYTE2] = 0x14;
	a314_state.shared_memory[CAP_BASE + RESTART_COUNTER] = a314_state.restart_counter;

	a314d_emu_init(a314_state.shared_memory, SHMEM_SIZE);

	write_log("A314: Initialization complete, clockport at 0x%06X\n", CLOCKPORT_ADDR);
}

void a314_reset(void)
{
	if (!a314_state.enabled)
		return;

	write_log("A314: Reset\n");

	a314_state.restart_counter++;
	a314_state.shared_memory[CAP_BASE + RESTART_COUNTER] = a314_state.restart_counter;

	a314_state.shared_memory[CAP_BASE + R2A_TAIL_OFFSET] = 0;
	a314_state.shared_memory[CAP_BASE + A2R_HEAD_OFFSET] = 0;
	a314_state.shared_memory[CAP_BASE + A2R_TAIL_OFFSET] = 0;
	a314_state.shared_memory[CAP_BASE + R2A_HEAD_OFFSET] = 0;

	a314_state.address_ptr = 0;
	a314_state.irq_status = 0;
	a314_state.amiga_irq_pending = false;

	a314d_emu_reset();
}

void a314_cleanup(void)
{
	if (!a314_state.enabled)
		return;

	write_log("A314: Cleanup\n");
	a314d_emu_cleanup();
	a314_state.enabled = false;
}

int a314_is_enabled(void)
{
	return a314_state.enabled ? 1 : 0;
}

void a314_rethink(void)
{
	// EXTER (INT6) is a shared, level-triggered interrupt. Re-assert our
	// request whenever the interrupt state is reconsidered, so it is not
	// permanently lost if another EXTER source (e.g. CIA-B) clears the shared
	// bit before the CPU takes it. Without this the a314 IRQ can be dropped on
	// some CPU/timing configs (seen hanging comms on 68020), because
	// a314_trigger_amiga_interrupt() only pulses INTREQ once.
	if (!a314_state.enabled)
		return;

	if (a314_state.amiga_irq_pending)
	{
		const int INTB_EXTER = 13;
		INTREQ_0(0x8000 | (1 << INTB_EXTER));
	}
}

void a314_hsync(void)
{
	// Runs every scanline on the main emulation thread. Assert EXTER if the
	// daemon thread flagged a pending interrupt (a314_trigger_amiga_interrupt).
	// This is the thread-safe replacement for the old cross-thread INTREQ, in
	// the same spirit as uaenet_int_requested handling in the hsync handler.
	if (!a314_state.enabled)
		return;

	if (a314_state.amiga_irq_pending)
	{
		const int INTB_EXTER = 13;
		INTREQ_0(0x8000 | (1 << INTB_EXTER));
	}
}

uae_u32 a314_bget(uaecptr addr)
{
	if (!a314_state.enabled)
		return 0;

	int reg = (addr - CLOCKPORT_ADDR) >> 2;

	uae_u32 value = 0;
	switch (reg)
	{
		case REG_SRAM:
			value = a314_state.shared_memory[a314_state.address_ptr];

			if (a314_state.address_ptr < 0x210)
				write_log_verbose("A314: get @ 0x%06X = %02x\n", a314_state.address_ptr, value);

			a314_state.address_ptr = (a314_state.address_ptr + 1) & 0xFFFF;
			break;

		case REG_IRQ:
			value = a314_state.irq_status;
			break;

		case REG_ADDR_LO:
			value = a314_state.address_ptr & 0xFF;
			break;

		case REG_ADDR_HI:
			value = (a314_state.address_ptr >> 8) & 0xFF;
			break;

		default:
			value = 0;
			break;
	}

	return value;
}

void a314_bput(uaecptr addr, uae_u32 value)
{
	if (!a314_state.enabled)
		return;

	int reg = (addr - CLOCKPORT_ADDR) >> 2;

	value &= 0xFF;

	switch (reg)
	{
		case REG_SRAM:
			a314_state.shared_memory[a314_state.address_ptr] = value;

			if (a314_state.address_ptr < 0x210)
				write_log_verbose("A314: put @ 0x%06X = %02x\n", a314_state.address_ptr, value);

			a314_state.address_ptr = (a314_state.address_ptr + 1) & 0xFFFF;
			break;

		case REG_IRQ:
			a314_state.irq_status = value;

			if ((value & REG_IRQ_SET) && (value & REG_IRQ_PI))
				a314d_emu_set_irq();

			if (!(value & REG_IRQ_SET) && (value & REG_IRQ_CP))
				a314_state.amiga_irq_pending = false;

			break;

		case REG_ADDR_LO:
			a314_state.address_ptr = (a314_state.address_ptr & 0xFF00) | value;
			break;

		case REG_ADDR_HI:
			a314_state.address_ptr = (a314_state.address_ptr & 0x00FF) | (value << 8);
			break;

		default:
			break;
	}
}

} // extern "C"

void a314_trigger_amiga_interrupt(void)
{
	if (!a314_state.enabled)
		return;

	// This runs on the a314 daemon thread. The emulator's interrupt/event
	// state (INTREQ, doint, the event queue) is NOT thread-safe, so we must not
	// touch it here. Just record that the a314 wants to interrupt; the main
	// emulation thread asserts EXTER from a314_hsync()/a314_rethink().
	a314_state.amiga_irq_pending = true;

	write_log_verbose("A314: Triggering Amiga interrupt (EXTER)\n");
}
