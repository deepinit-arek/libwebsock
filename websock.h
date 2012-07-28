//libwebsock Copyright 2012 Payden Sutherland

#define EPOLL_EVENTS 100
#define PORT_STRLEN 12
#define LISTEN_BACKLOG 10
#define FRAME_CHUNK_LENGTH 1024
#define MASK_LENGTH 4

typedef struct _libwebsock_frame {
	unsigned int fin;
	unsigned int opcode;
	unsigned int mask_offset;
	unsigned int payload_offset;
	unsigned int rawdata_idx;
	unsigned int rawdata_sz;
	unsigned int complete;
	unsigned long long payload_len;
	char *rawdata;
	struct _libwebsock_frame *next_frame;
	struct _libwebsock_frame *prev_frame;
	unsigned char mask[4];
} libwebsock_frame;


typedef struct {
	unsigned int opcode;
	unsigned long long payload_len;
	char *payload;
} libwebsock_message;

typedef struct {
	int listen_fd;
	int epoll_fd;
	int (*received_callback)(int, libwebsock_message*);
	char port[PORT_STRLEN];
	struct epoll_event *events;
} libwebsock_context;

typedef struct {
	int sockfd;
	int num_frames;
	char *data;
	libwebsock_frame *current_frame;
} libwebsock_client_state;

//function defs

int libwebsock_send_text(int sockfd, char *strdata);
int libwebsock_complete_frame(libwebsock_frame *frame);
void libwebsock_dispatch_message(libwebsock_context *ctx, libwebsock_client_state *state, libwebsock_frame *current);
void libwebsock_in_data(libwebsock_context *ctx, libwebsock_client_state *state, char byte);
void libwebsock_dump_frame(libwebsock_frame *frame);
void libwebsock_handle_recv(libwebsock_context *ctx, libwebsock_client_state *state, char *data, int datalen);
void libwebsock_handle_client_event(libwebsock_context *ctx, libwebsock_client_state *state);
void libwebsock_wait(libwebsock_context *ctx);
void libwebsock_handshake(libwebsock_context *ctx, int sockfd);
void libwebsock_set_receive_cb(libwebsock_context *ctx, int (*cb)(int, libwebsock_message *msg));
libwebsock_context *libwebsock_init(char *port);