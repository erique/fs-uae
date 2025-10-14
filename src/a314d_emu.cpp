/*
 * A314 Daemon Emulation
 *
 * This implements the daemon side of the A314 protocol, adapted from
 * the original a314d.cc but running as a thread within FS-UAE instead
 * of as a separate process on a Raspberry Pi.
 *
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "a314.h"
#include "threaddep/thread.h"

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

void a314_trigger_amiga_interrupt(void);

static struct
{
	bool running;
	uae_thread_id thread;
	uae_u8 *shared_memory;
	int shmem_size;

	void (*interrupt_callback)(void);

	int server_socket;

	int irq_pipe[2];  /* [0] = read end, [1] = write end */

} a314d_state;

#define logger_trace(...)   do { /*write_log("A314D TRACE: "); write_log(__VA_ARGS__);*/ } while (0)
#define logger_debug(...)   do { /*write_log("A314D DEBUG: "); write_log(__VA_ARGS__);*/ } while (0)
#define logger_info(...)    do { write_log("A314D INFO: ");  write_log(__VA_ARGS__); } while (0)
#define logger_warning(...) do { write_log("A314D WARN: ");  write_log(__VA_ARGS__); } while (0)
#define logger_error(...)   do { write_log("A314D ERROR: "); write_log(__VA_ARGS__); } while (0)

static inline void read_shm(uint8_t *data, unsigned int address, unsigned int length)
{
	if (address + length > a314d_state.shmem_size)
	{
		logger_warning("read_shm out of bounds: addr=0x%x len=%d\n", address, length);
		return;
	}
	memcpy(data, &a314d_state.shared_memory[address], length);
}

static inline void write_shm(unsigned int address, uint8_t *data, unsigned int length)
{
	if (address + length > a314d_state.shmem_size)
	{
		logger_warning("write_shm out of bounds: addr=0x%x len=%d\n", address, length);
		return;
	}
	memcpy(&a314d_state.shared_memory[address], data, length);
}

void a314d_emu_set_irq(void)
{
	if (a314d_state.irq_pipe[1] != -1)
	{
		char dummy = 1;
		write(a314d_state.irq_pipe[1], &dummy, 1);
	}
}

static void clear_pi_irq()
{
	char buf[64];
	while (read(a314d_state.irq_pipe[0], buf, sizeof(buf)) > 0)
		;
}

static void set_cp_irq(void)
{
	a314_trigger_amiga_interrupt();
}

// fake epoll
struct epoll_event
{
	uint32_t      events;
};
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008

#define MODEL_CP 1
#define UAE_MODE 1
#include "a314d.cc"

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

static void* thread_func(void *arg);

void a314d_emu_init(uae_u8 *shmem, int shmem_size)
{
	logger_info("Initializing daemon emulation\n");

	memset(&a314d_state, 0, sizeof(a314d_state));
	a314d_state.shared_memory = shmem;
	a314d_state.shmem_size = shmem_size;
	a314d_state.running = false;
	a314d_state.server_socket = -1;
	a314d_state.irq_pipe[0] = -1;
	a314d_state.irq_pipe[1] = -1;

	if (pipe(a314d_state.irq_pipe) < 0)
	{
		logger_error("Failed to create IRQ pipe\n");
		return;
	}

	fcntl(a314d_state.irq_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(a314d_state.irq_pipe[1], F_SETFL, O_NONBLOCK);

	const char *config_paths[] =
	{
		"a314d.conf",
		"a314/a314d.conf",
		NULL
	};

	for (int i = 0; config_paths[i] != NULL; i++)
	{
		FILE *test = fopen(config_paths[i], "r");
		if (test)
		{
			fclose(test);
			load_config_file(config_paths[i]);
			break;
		}
	}

	a314d_state.server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (a314d_state.server_socket == -1)
	{
		logger_error("Failed to create server socket\n");
		return;
	}

	int reuse = 1;
	setsockopt(a314d_state.server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	fcntl(a314d_state.server_socket, F_SETFL, O_NONBLOCK);

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(7110);

	if (::bind(a314d_state.server_socket, (struct sockaddr *)&address, sizeof(address)) < 0)
	{
		logger_error("Failed to bind to localhost:7110 (errno=%d)\n", errno);
		close(a314d_state.server_socket);
		a314d_state.server_socket = -1;
		return;
	}

	if (listen(a314d_state.server_socket, 16) < 0)
	{
		logger_error("Failed to listen\n");
		close(a314d_state.server_socket);
		a314d_state.server_socket = -1;
		return;
	}

	a314d_state.running = true;
	uae_start_thread("a314d", thread_func, NULL, &a314d_state.thread);

	logger_info("Daemon thread started, listening on localhost:7110\n");
}


void a314d_emu_reset(void)
{
	logger_info("Reset\n");
	close_all_logical_channels();
}

void a314d_emu_cleanup(void)
{
	logger_info("Cleanup\n");

	a314d_state.running = false;

	if (a314d_state.thread)
	{
		uae_wait_thread(a314d_state.thread);
		a314d_state.thread = 0;
	}

	if (a314d_state.server_socket != -1)
	{
		close(a314d_state.server_socket);
		a314d_state.server_socket = -1;
	}

	if (a314d_state.irq_pipe[0] != -1)
	{
		close(a314d_state.irq_pipe[0]);
		a314d_state.irq_pipe[0] = -1;
	}

	if (a314d_state.irq_pipe[1] != -1)
	{
		close(a314d_state.irq_pipe[1]);
		a314d_state.irq_pipe[1] = -1;
	}

	while (!connections.empty())
		close_and_remove_connection(&connections.front());
}


static void* thread_func(void *arg)
{
	logger_info("Daemon thread running\n");

	handle_a314_irq();

	while (a314d_state.running)
	{
		fd_set readfds;
		FD_ZERO(&readfds);

		int max_fd = -1;

		if (a314d_state.irq_pipe[0] != -1) {
			FD_SET(a314d_state.irq_pipe[0], &readfds);
			if (a314d_state.irq_pipe[0] > max_fd)
				max_fd = a314d_state.irq_pipe[0];
		}

		if (a314d_state.server_socket != -1) {
			FD_SET(a314d_state.server_socket, &readfds);
			if (a314d_state.server_socket > max_fd)
				max_fd = a314d_state.server_socket;
		}

		for (auto &cc : connections) {
			FD_SET(cc.fd, &readfds);
			if (cc.fd > max_fd)
				max_fd = cc.fd;
		}

		struct timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		int n = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

		if (n == -1) {
			if (errno == EINTR)
				continue;
			logger_error("select() failed with errno=%d\n", errno);
			break;
		}
		else if (n == 0) {
			continue;
		}

		if (a314d_state.irq_pipe[0] != -1 && FD_ISSET(a314d_state.irq_pipe[0], &readfds))
			handle_a314_irq();

		if (a314d_state.server_socket != -1 && FD_ISSET(a314d_state.server_socket, &readfds))
			handle_server_socket_ready();

		for (auto it = connections.begin(); it != connections.end(); )
		{
			if (FD_ISSET(it->fd, &readfds))
			{
				ClientConnection *cc = &(*it);

				auto curr_it = it;
				++it;

				{
					epoll_event ev;
					ev.events = EPOLLIN | EPOLLOUT;
					handle_client_connection_event(cc, &ev);
				}

				if (curr_it != it)
					continue;
			}
			else
			{
				++it;
			}
		}

		flush_send_queue();
		write_channel_status();
	}

	logger_info("Daemon thread exiting\n");
	return NULL;
}
