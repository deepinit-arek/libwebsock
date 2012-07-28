#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "websock.h"
#include "sha1.h"
#include "base64.h"

void libwebsock_wait(libwebsock_context *ctx) {
	int ret, i, new_fd;
	socklen_t sin_size;
	libwebsock_client_state *client_state = NULL;
	struct sockaddr_storage theiraddr;
	while((ret = epoll_wait(ctx->epoll_fd, ctx->events, EPOLL_EVENTS, 1000)) >= 0) {
		for(i = 0; i < ret; i++) {
			if(!ctx->events[i].data.ptr) {
				//accepting new connection.
				new_fd = accept(ctx->listen_fd, (struct sockaddr *)&theiraddr, &sin_size);
				if(new_fd != -1) {
					fprintf(stderr, "DEBUG: Accepted new connection, performing handshake.\n");
					libwebsock_handshake(ctx, new_fd);	
				}
			}
			else {
				client_state = (libwebsock_client_state *)ctx->events[i].data.ptr;
				libwebsock_handle_client_event(ctx, client_state);
			}
		}
	}
}

int libwebsock_send_text(int sockfd, char *strdata)  {
	if(strdata == NULL) {
		fprintf(stderr, "Will not send empty message.\n");
		return -1;
	}
	unsigned long long payload_len;
	unsigned char finNopcode;
	unsigned int payload_len_small;
	unsigned int payload_offset = 2;
	unsigned int len_size;
	unsigned long long be_payload_len;
	unsigned int sent = 0;
	int i;
	size_t frame_size;
	char *data;
	payload_len = strlen(strdata);
	finNopcode = 0x81; //FIN and text opcode.
	if(payload_len <= 125) {
		frame_size = 2 + payload_len;
		data = (void *)malloc(frame_size);
		payload_len_small = payload_len;
	} else if(payload_len > 125 && payload_len <= 0xffff) {
		frame_size = 4 + payload_len;
		data = (void *)malloc(frame_size);
		payload_len_small = 126;
		payload_offset += 2;
	} else if(payload_len > 0xffff && payload_len <= 0xffffffffffffffff) {
		frame_size = 10 + payload_len;
		data = (void *)malloc(frame_size);
		payload_len_small = 127;
		payload_offset += 8;
	} else {
		fprintf(stderr, "Whoa man.  What are you trying to send?\n");
		return -1;
	}
	memset(data, 0, frame_size);
	payload_len_small &= 0x7f;
	memcpy(data, &finNopcode, 1);
	memcpy(data+1, &payload_len_small, 1); //mask bit off, 7 bit payload len
	if(payload_len_small == 126) {
		payload_len &= 0xffff;
		len_size = 2;
		for(i = 0; i < len_size; i++) {
			memcpy(data+2+i, (void *)&payload_len+(len_size-i-1), 1);
		}
	}
	if(payload_len_small == 127) {
		payload_len &= 0xffffffffffffffff;
		len_size = 8;
		for(i = 0; i < len_size; i++) {
			memcpy(data+2+i, (void *)&payload_len+(len_size-i-1), 1);
		}
	}
	memcpy(data+payload_offset, strdata, strlen(strdata));
	sent = 0;
	
	while(sent < frame_size) {
		sent += send(sockfd, data+sent, frame_size, 0);
	}
	free(data);
	return 1;
}

//used for debugging purposes
void libwebsock_dump_frame(libwebsock_frame *frame) {
	fprintf(stderr, "FIN: %d\n", frame->fin);
	fprintf(stderr, "Opcode: %d\n", frame->opcode);
	fprintf(stderr, "mask_offset: %d\n", frame->mask_offset);
	fprintf(stderr, "payload_offset: %d\n", frame->payload_offset);
	fprintf(stderr, "rawdata_idx: %d\n", frame->rawdata_idx);
	fprintf(stderr, "rawdata_sz: %d\n", frame->rawdata_sz);
	fprintf(stderr, "complete: %d\n", frame->complete);
	fprintf(stderr, "payload_len: %llu\n", frame->payload_len);
	fprintf(stderr, "Has previous frame: %d\n", frame->prev_frame != NULL ? 1 : 0);
	fprintf(stderr, "Has next frame: %d\n", frame->next_frame != NULL ? 1 : 0);
	fprintf(stderr, "Raw data:\n");
	int i;
	fprintf(stderr, "%02x", *(frame->rawdata) & 0xff);
	for(i=1;i<frame->rawdata_idx;i++) {
		fprintf(stderr, ":%02x", *(frame->rawdata+i) & 0xff);
	}
	fprintf(stderr, "\n");
}

void libwebsock_free_all_frames(libwebsock_client_state *state) {
	libwebsock_frame *current;
	if(state != NULL) {
		if(state->current_frame != NULL) {
			current = state->current_frame;
			for(;current->prev_frame != NULL;current = current->prev_frame) {}
			for(;current != NULL; current = current->next_frame) {
				if(current->prev_frame != NULL) {
					free(current->prev_frame);
				}
				if(current->rawdata != NULL) {
					free(current->rawdata);
				}
			}
			free(current);
		}
	}
}
				

void libwebsock_handle_client_event(libwebsock_context *ctx, libwebsock_client_state *state) {
	char buf[1024];
	char *newdata = NULL;
	int n;
	memset(buf, 0, 1024);
	n = recv(state->sockfd, buf, 1023, 0);
	if(n == 0) {
		fprintf(stderr, "Client closed connection.\n");
		libwebsock_free_all_frames(state);
		close(state->sockfd);
		free(state);
		return;
	}
	newdata = (char *)malloc(n+1);
	if(!newdata) {
		fprintf(stderr, "Unable to allocate memory in libwebsock_handle_client_event\n");
		exit(1);
	}
	memset(newdata, 0, n+1);
	memcpy(newdata, buf, n);
	libwebsock_handle_recv(ctx, state, newdata, n);

}

void libwebsock_handle_recv(libwebsock_context *ctx, libwebsock_client_state *state, char *data, int datalen) {
	//alright... while we haven't reached the end of data keep trying to build frames
	//possible states right now:
	// 1.) we're receiving the beginning of a new frame
	// 2.) we're receiving more data from a frame that was created previously and was not complete
	int i;
	for(i=0;i<datalen;i++) {
		libwebsock_in_data(ctx, state, *(data+i));
	}
	free(data);

}

void libwebsock_in_data(libwebsock_context *ctx, libwebsock_client_state *state, char byte) {
	libwebsock_frame *current = NULL, *new = NULL;
	unsigned char payload_len_short;
	if(state->current_frame == NULL) {
		state->current_frame = (libwebsock_frame *)malloc(sizeof(libwebsock_frame));
		memset(state->current_frame, 0, sizeof(libwebsock_frame));
		state->current_frame->payload_len = -1;
		state->current_frame->rawdata_sz = FRAME_CHUNK_LENGTH;
		state->current_frame->rawdata = (char *)malloc(state->current_frame->rawdata_sz);
		memset(state->current_frame->rawdata, 0, state->current_frame->rawdata_sz);
	}
	current = state->current_frame;
	if(current->rawdata_idx >= current->rawdata_sz) {
		current->rawdata_sz += FRAME_CHUNK_LENGTH;
		current->rawdata = (char *)realloc(current->rawdata, current->rawdata_sz);
		memset(current->rawdata + current->rawdata_idx, 0, current->rawdata_sz - current->rawdata_idx);
	}
	*(current->rawdata + current->rawdata_idx++) = byte;
	if(libwebsock_complete_frame(current) == 1) {
		state->num_frames++;
		if(current->fin == 1) {
			libwebsock_dispatch_message(ctx, state, current);
			state->current_frame = NULL;
		} else {
			new = (libwebsock_frame *)malloc(sizeof(libwebsock_frame));
			memset(new, 0, sizeof(libwebsock_frame));
			new->payload_len = -1;
			new->rawdata = (char *)malloc(FRAME_CHUNK_LENGTH);
			memset(new->rawdata, 0, FRAME_CHUNK_LENGTH);
			new->prev_frame = current;
			current->next_frame = new;
			state->current_frame = new;
		}
	}
}

void libwebsock_cleanup_frames(libwebsock_frame *first) {
	libwebsock_frame *this = NULL;
	libwebsock_frame *next = first;
	while(next != NULL) {
		this = next;
		next = this->next_frame;
		if(this->rawdata != NULL) {
			free(this->rawdata);
		}
		free(this);
	}
}

void libwebsock_dispatch_message(libwebsock_context *ctx, libwebsock_client_state *state, libwebsock_frame *current) {
	unsigned long long message_payload_len, message_offset;
	int message_opcode, i;
	char *message_payload;
	libwebsock_frame *first = NULL;
	libwebsock_message *msg = NULL;
	if(current == NULL) {
		fprintf(stderr, "Somehow, null pointer passed to libwebsock_dispatch_message.\n");
		exit(1);
	}
	message_offset = 0;
	message_payload_len = current->payload_len;
	for(;current->prev_frame != NULL;current = current->prev_frame) {
		message_payload_len += current->payload_len;
	}
	first = current;
	message_opcode = current->opcode;
	message_payload = (char *)malloc(message_payload_len + 1);
	memset(message_payload, 0, message_payload_len + 1);
	for(;current != NULL; current = current->next_frame) {
		for(i = 0; i < current->payload_len; i++) {
			//demask frame payload
			*(current->rawdata + current->payload_offset + i) ^= (current->mask[i % 4] & 0xff);
		}
		memcpy(message_payload + message_offset, current->rawdata + current->payload_offset, current->payload_len);
		message_offset += current->payload_len;
	}

	libwebsock_cleanup_frames(first);

	msg = (libwebsock_message *)malloc(sizeof(libwebsock_message));
	memset(msg, 0, sizeof(libwebsock_message));
	msg->opcode = message_opcode;
	msg->payload_len = message_offset;
	msg->payload = message_payload;
	if(ctx->received_callback != NULL) {
		ctx->received_callback(state->sockfd, msg);
	} else {
		fprintf(stderr, "No received call back registered with libwebsock.\n");
	}
	free(msg->payload);
	free(msg);
}

int libwebsock_complete_frame(libwebsock_frame *frame) {
	int payload_len_short, i;
	unsigned long long payload_len = 0;
	if(frame->rawdata_idx < 2) {
		return 0;
	}
	frame->mask_offset = 2;
	frame->fin = (*(frame->rawdata) & 0x80) == 0x80 ? 1 : 0;
	frame->opcode = *(frame->rawdata) & 0x0f;
	if((*(frame->rawdata+1) & 0x80) != 0x80) {
		fprintf(stderr, "Received unmasked frame from client.  Fail this in the future.\n");
		exit(1);
	}
	payload_len_short = *(frame->rawdata+1) & 0x7f;
	switch(payload_len_short) {
	case 126:
		if(frame->rawdata_idx < 4) {
			fprintf(stderr, "Frame has 16 bit payload len, but not enough bytes to read it yet.\n");
			return 0;
		}
		for(i = 0; i < 2; i++) {
			memcpy((void *)&payload_len+i, frame->rawdata+3-i, 1);
		}
		frame->mask_offset += 2;
		frame->payload_len = payload_len;
		break;
	case 127:
		if(frame->rawdata_idx < 10) {
			fprintf(stderr, "Frame has 64 bit payload len, but not enough bytes to read it yet.\n");
			return 0;
		}
		for(i = 0; i < 8; i++) {
			memcpy((void *)&payload_len+i, frame->rawdata+9-i, 1);
		}
		frame->mask_offset += 8;
		frame->payload_len = payload_len;
		break;
	default:
		frame->payload_len = payload_len_short;
		break;

	}
	frame->payload_offset = frame->mask_offset + MASK_LENGTH;
	if(frame->rawdata_idx < frame->payload_offset + frame->payload_len) {
		return 0;
	}
	for(i = 0; i < MASK_LENGTH; i++) {
		frame->mask[i] = *(frame->rawdata + frame->mask_offset + i) & 0xff;
	}
	return 1;
}

void libwebsock_handshake(libwebsock_context *ctx, int sockfd) {
	//probably shouldn't have a static size for handshake buffer, maybe some better programmers can learn me in this.
	char buf[1024];
	char sha1buf[45];
	unsigned char sha1mac[20];
	char *concat = NULL;
	char *base64buf = NULL;
	const char *GID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	struct epoll_event ev;
	libwebsock_client_state *state = NULL;
	SHA1Context shactx;
	SHA1Reset(&shactx);
	memset(buf, 0, 1024);
	int n = 0;
	int x = 0;
	int endheader = 0;
	while(endheader == 0) {
		n = recv(sockfd, &buf[x], 1023, 0);
		if(strcmp(&buf[x+(n-4)], "\r\n\r\n") == 0) {
			endheader = 1;
		}
		x += n;
	}
	
	char *tok = NULL, *headers = NULL, *key = NULL;
	headers = (char *)malloc(1024);
	if(!headers) {
		fprintf(stderr, "Unable to allocate memory in libwebsock_handshake..\n");
		close(sockfd);
		return;
	}
	strncpy(headers, buf, 1023);
	for(tok = strtok(headers, "\r\n"); tok != NULL; tok = strtok(NULL, "\r\n")) {
		if(strstr(tok, "Sec-WebSocket-Key: ") != NULL) {
			key = (char *)malloc(strlen(tok));
			strncpy(key, tok+strlen("Sec-WebSocket-Key: "), strlen(tok));
			break;
		}
	}

	
	if(key == NULL) {
		fprintf(stderr, "Unable to find key in request headers.\n");
		close(sockfd);
		return;
	}

	concat = (char *)malloc(strlen(GID) + strlen(key) + 1);
	strncpy(concat, key, strlen(key));
	strncat(concat, GID, strlen(GID));
	SHA1Input(&shactx, (unsigned char *)concat, strlen(concat));
	SHA1Result(&shactx);
	free(concat);
	free(key);
	key = concat = NULL;
	sprintf(sha1buf, "%08x%08x%08x%08x%08x", shactx.Message_Digest[0], shactx.Message_Digest[1], shactx.Message_Digest[2], shactx.Message_Digest[3], shactx.Message_Digest[4]);
	for(n = 0; n < (strlen(sha1buf)/2);n++)
		sscanf(sha1buf+(n*2), "%02hhx", sha1mac+n);
	base64buf = (char *)malloc(256);
	base64_encode(sha1mac, 20, base64buf, 256);
	memset(buf, 0, 1024);
	snprintf(buf, 1024, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", base64buf);
	for(n = 0; n < strlen(buf);)
		n += send(sockfd, buf+n, strlen(buf+n), 0);
	state = (libwebsock_client_state *)malloc(sizeof(libwebsock_client_state));
	memset(state, 0, sizeof(libwebsock_client_state));
	state->sockfd = sockfd;
	ev.data.ptr = state;
	ev.events = EPOLLIN;
	epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, sockfd, &ev);
}

void libwebsock_set_receive_cb(libwebsock_context *ctx, int (*cb)(int, libwebsock_message* msg)) {
	ctx->received_callback = cb;
}

libwebsock_context *libwebsock_init(char *port) {
	libwebsock_context *ctx;
	struct addrinfo hints, *servinfo = NULL, *p = NULL;
	struct epoll_event ev;
	int yes = 1;
	ctx = (libwebsock_context *)malloc(sizeof(libwebsock_context));
	if(!ctx) {
		fprintf(stderr, "Unable to allocate memory for libwebsock context.\n");
		return ctx;
	}
	memset(ctx, 0, sizeof(libwebsock_context));
	strncpy(ctx->port, port, PORT_STRLEN);
	if((ctx->epoll_fd = epoll_create(EPOLL_EVENTS)) == -1) {
		perror("epoll");
		free(ctx);
		ctx = NULL;
		return ctx;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if((getaddrinfo(NULL, ctx->port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo failed\n");
		free(ctx);
		ctx = NULL;
		return ctx;
	}
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if((ctx->listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
			perror("socket");
			continue;
		}
		if(setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
			perror("setsockopt");
			free(ctx);
			ctx = NULL;
			return ctx;
		}
		if(bind(ctx->listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
			close(ctx->listen_fd);
			perror("bind");
			continue;
		}
		break;
	}
	
	if(p == NULL) {
		fprintf(stderr, "Failed to bind..\n");
		free(ctx);
		ctx = NULL;
		return ctx;
	}

	freeaddrinfo(servinfo);
	
	if(listen(ctx->listen_fd, LISTEN_BACKLOG) == -1) {
		perror("listen");
		exit(1);
	}

	ev.data.ptr = NULL;
	ev.events = EPOLLIN;
	if(epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) == -1) {
		perror("epoll_ctl");
		free(ctx);
		ctx = NULL;
		return ctx;
	}
	ctx->events = (struct epoll_event *)malloc(sizeof(struct epoll_event) * EPOLL_EVENTS);
	if(!ctx->events) {
		fprintf(stderr, "Unable to allocate memory for epoll events queue.\n");
		free(ctx);
		ctx = NULL;
		return ctx;
	}
	return ctx;
}