#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define MAX_ERROR_LENGTH 1000

#define MAX_EVENT_ID_LENGTH 100
#define MAX_INCOMING_REQUEST_SIZE 512

#define RESEND_TIME 1

#define REQUEST_RECV_PORT 5654
#define RESPONSE_RECV_PORT 5655

/* list head makes a list */
typedef struct list_head {
	struct list_head *next;
	struct list_head *prev;
}list_head_t;

/* list holds the start and end of a list */
typedef struct list {
	struct list_head *start;
	struct list_head *end;
}list_t;

#define RESET_NODE(head)          do{ (head)->next = NULL; (head)->prev = NULL; \
                                    }while(0)
#define ADD_NEXT(node, new)       do{ (new)->next = (node)->next; if ((new)->next != NULL) { \
                                      (new)->next->prev = new;} (new)->prev = node; \
                                      (node)->next = new; \
                                    }while(0)
#define ADD_PREV(node, new)       do{ (new)->prev = (node)->prev; if ((new)->prev != NULL) { \
                                      (new)->prev->next = new;} (new)->next = (node); \
                                      (node)->prev = new; \
                                    }while(0)
#define UNLINK_NODE(node)         do{ \
					if((node)->next != NULL) { \
						(node)->next->prev = (node)->prev; \
                                      	} \
				      	if((node)->prev != NULL) { \
                                      		(node)->prev->next = (node)->next; \
				      	} \
					RESET_NODE(node); \
                                    }while(0)
#define ADD_START(list, node)     do{ if(IS_EMPTY(list)) { (list)->start = node; (list)->end = node; break;} \
				      ADD_PREV((list)->start, node); (list)->start = node; \
                                    }while(0)
#define ADD_END(list, node)       do{ if(IS_EMPTY(list)) { (list)->end = node; (list)->start = node; break;} \
				      ADD_NEXT(((list)->end), (node)); (list)->end = node; \
                                    }while(0)

#define IS_EMPTY(list)            ((list)->start == NULL && (list)->end == NULL)

#define CAST_OUT(ptr, type)       ((type *)((char*)ptr - (sizeof(type) - sizeof(list_head_t))))

/************************************* Linker Global Definition ******************************/

typedef enum role {
	DEAMON, // runs as a deamon and executes incoming request
	WAITFOR // wair for a set for service on a specific event
}role_t;

/* event task is a waitfor task object */
typedef struct event_task {
	struct sockaddr_in sa;
	char *event_id;
	// node holds event_task into a list
	list_head_t node;
}event_task_t;

/* event script to map a script against an id */
typedef struct event_script {
	char *event_id;
	char *script_path;
	// node holds event_script into a list
	list_head_t node;
}event_script_t;

/* poll request is a script execution request */
/*
typedef struct poll_request {
	char *event_id;
	struct sockaddr_in client_addr;
       	char *script_path;
	// node holds poll_reqyest into a list
	list_head_t node;
}poll_request_t;
*/

/* request/response body */
typedef struct reqresp_body {
	unsigned int is_req;
	char event_id[MAX_EVENT_ID_LENGTH];
}reqresp_body_t;


/************************************* Linker Global Variables *******************************/

// Holds the list of events for which the linker is waiting 
static list_t poll_event_list;
// Holds the list of event-script mappings
static list_t event_script_map_list; 
// Holds the list of events that has been requested
static list_t request_queue;

// Lock to sync poll_event_list
static pthread_mutex_t poll_event_list_lock;

// FFlag to stop threads
int stop = 0;
// port
static int port = 0; 
// port to consider while communicating with peer
static int peer_port = 0; 
// role
static role_t role;

// threads
// poller polls for events in wating mode
pthread_t poller_thread;
// reader reads response or request in both waiting and deamon mode
pthread_t reader_thread;

// Listen fd
int fd;
// error no
extern int errno;
// Hold error string
static char error[MAX_ERROR_LENGTH];

#define getError() (error)

void set_error_s(char *dest, char *error_fmt, ...) {
	va_list valist;
	va_start(valist, error_fmt);
        // Set error string
	vsnprintf(dest, MAX_ERROR_LENGTH -1, error_fmt, valist);
	va_end(valist); 
} 

void set_error(char *error_fmt, ...) {
	va_list valist;
	va_start(valist, error_fmt);
        // Set error string
	vsnprintf(error, MAX_ERROR_LENGTH -1, error_fmt, valist);
	va_end(valist); 
} 


int init(void) {
	poll_event_list.start = NULL;
	poll_event_list.end = NULL;
	request_queue.start = NULL;
	request_queue.end = NULL;
	event_script_map_list.start = NULL;
	event_script_map_list.end = NULL;

	pthread_mutex_init(&poll_event_list_lock, NULL);
	return 0;
}

void print_help(char *exec_name) {
	printf("\n%s [opt] [event:script].. [service:event]..\n \
		\nopt: \n  \
		-h \t\t: print help\n \
		-r <role> \t: set role [deamon/waitfor] \n \
		-p <port> \t: listen port\n \
		-P <peerport> \t: peer port (port to send request/response)\n \
		example:\n \
		%s -p 5000 -r deamon  mysql_start:/usr/local/mysql_start_check.sh \n \
                %s -p 4000 -P 5000 -r waitfor mysql:mysql_start \n",
		exec_name, exec_name, exec_name
	      );
}

// register a script for an event
int register_event_script(char *arg) {
	char *token, *str, *tofree;
	char *event, *script;
	int index = 0;
	event_script_t *es_node;
	tofree = str = strdup(arg);
	while ((token = strsep(&str, ":"))) {
		switch(index) {
			case 0:
				event = strdup(token);	
				break;
			case 1:
				script = strdup(token);
		}
		index++;
		if (index > 1) {
			break;
		}
	}
	free(tofree);

    	// Check if the script file exist
    	if(access(script, F_OK) == -1) {
		set_error("Can't access file :%s\n", script);
		return 1;
	}

	// Create event script node
	es_node = (event_script_t *)malloc(sizeof(event_script_t));
	es_node->event_id = event;
	es_node->script_path = script;

	// Add node to event script list
	RESET_NODE(&es_node->node);
	ADD_END(&event_script_map_list, &es_node->node);
		
	return 0;
}

// fimds a event_script object based on event id
event_script_t * search_event_script(char *event_id) {
	list_head_t *es_node;
	event_script_t *es_body = NULL;
	for(es_node = event_script_map_list.start;;) {
		if (es_node == NULL) {
			break;
		}
		es_body = CAST_OUT(es_node, event_script_t);
		if (strcmp(es_body->event_id, event_id) == 0) {
			return es_body;
		}
		es_node = es_node->next;
	}
	return NULL;
}

// clean an event script list
void clean_event_script_map_list(void) {
	list_head_t *es_node;
	list_head_t *es_next_node;
	event_script_t *es_body;
	
	for(es_node = event_script_map_list.start; es_node != event_script_map_list.end;) {
		if (es_node == NULL) {
			break;
		}
		es_next_node = es_node->next;
		es_body = CAST_OUT(es_node, event_script_t);
		free(es_body->event_id);
		free(es_body->script_path);
		UNLINK_NODE(es_node);
		free(es_body);
		es_node = es_next_node;
	}
}

// add a service and event to poll for
int register_poll_event(char *arg) {
	char *token, *str, *tofree;
	char *event, *service;
	int index = 0;
	event_task_t *et_node;
	struct hostent * he;
	tofree = str = strdup(arg);
	struct in_addr  addr;
	while ((token = strsep(&str, ":"))) {
		switch(index) {
			case 0:
				service = strdup(token);	
				break;
			case 1:
				event = strdup(token);
		}
		index++;
		if (index > 1) {
			break;
		}
	}
	free(tofree);

    	// get the service address
    	do {
		he = gethostbyname(service);  
		if(he == NULL) {
			set_error("service can't be resolved '%s'\n", service);
			sleep(1);
			continue;
		}

		if (he->h_addr_list[0] == NULL) { 
			set_error("service can't be resolved '%s'\n", service);
			sleep(1);
			continue;
		}
	}while(0);
	
	// Create event script node
	et_node = (event_task_t *)malloc(sizeof(event_task_t));
	et_node->event_id = event;
	memcpy(&(et_node->sa.sin_addr), he->h_addr_list[0], he->h_length);
	et_node->sa.sin_family = AF_INET;
	et_node->sa.sin_port = htons(peer_port);

	// Add node to event script list
	RESET_NODE(&et_node->node);
	ADD_END(&poll_event_list, &(et_node->node));
		
	printf("registered service '%s' as '%s:%d' for event '%s'\n", service, inet_ntoa(et_node->sa.sin_addr),
									peer_port, event);

	return 0;
}

// search event from poll event list
event_task_t *search_poll_event(char *event_id) {
	list_head_t *et_node;
	event_task_t *et_body = NULL;
	for(et_node = poll_event_list.start;;) {
		if (et_node == NULL) {
			break;
		}
		et_body = CAST_OUT(et_node, event_task_t);
		if (strcmp(et_body->event_id, event_id) == 0) {
			return et_body;
		}
		et_node = et_node->next;
	}
	return NULL;
}

// delete a poll event task from event list
void delete_poll_event(event_task_t *et_body) {
	// check if start
	if(&(et_body->node)== poll_event_list.start) {
		poll_event_list.start = et_body->node.next;
	}
	// check if end
	if(&(et_body->node) == poll_event_list.end) {
		poll_event_list.end = et_body->node.prev;
	}
	// unlink the node
	UNLINK_NODE(&et_body->node); 
}


// clean the event script list
void clean_poll_event_list(void) {
	list_head_t *et_node;
	list_head_t *et_next_node;
	event_task_t *et_body;
	
	for(et_node = poll_event_list.start; et_node != poll_event_list.end;) {
		if (et_node == NULL) {
			break;
		}
		et_next_node = et_node->next;
		et_body = CAST_OUT(et_node, event_task_t);
		free(et_body->event_id);
		UNLINK_NODE(et_node);
		free(et_body);
		et_node = et_next_node;
	}
}

// add a poll request to request queue
/*
int add_poll_request(char *event_id, struct sockaddr_in *ca) {
	poll_event_t *request_node;
	event_script_t *es_node;

	es_node = search_event_script(event_id);
	if (es_node == NULL) {
		fprintf(stderr, "Invalid request, unknown event: %s\n", event_id);
		return 1;
	}
	
	// Create a poll request node
	request_node = (poll_request_t *)malloc(sizeof(poll_request_t));
	request_node->event_id = strdup(event_id);
	request_node->script_path = strdup(es_node->script_path);
	memcpy(&request_node->client_addr, ca, sizeof(struct sockaddr_in));

	// Add node to request queue
	ADD_START(&request_queue, &request_node->node);
	if (request_queue.end == NULL)
		request_queue.end = &request_node->node;
		
	return 0;
}*/

// clean the request queue
/*
void clean_poll_request_queue(void) {
	list_head_t *poll_node;
	list_head_t *poll_next_node;
	poll_request_t *poll_request;
	
	for(poll_node = request_queue.start; poll_node != request_queue.end;) {
		if (poll_node == NULL) {
			break;
		}
		poll_next_node = poll_node->next;
		poll_request = CAST_OUT(poll_node, poll_request_t);
		free(poll_request->event_id);
		UNLINK_NODE(poll_node);
		free(poll_request);
		poll_node = poll_next_node;
	}
}*/


int cleanup(void) {
	// clean retered event script
	clean_event_script_map_list();
	// clean service event
	clean_poll_event_list();
}

int parse_argument(int argc, char **argv) {
	int c;
        char *cvalue = NULL;
        int index;

	if (argc == 1) {
		set_error("Not sufficient argument provided\n");
		return 1;
	}

	while ((c = getopt (argc, argv, "hr:p:P:")) != -1){
    		switch (c) {
			case 'h':
				print_help(argv[0]);
        			exit(0);
      			case 'r':
        			if(strcmp(optarg, "deamon") == 0) {
					printf("starting linker as deamon\n");
					role = DEAMON;
					if (port == 0)
						port = REQUEST_RECV_PORT;
					if (peer_port == 0)
						peer_port = RESPONSE_RECV_PORT;
				} else if(strcmp(optarg, "waitfor") == 0) {
					printf("starting linker to waitfor\n");
					role = WAITFOR;
					if(port == 0)
						port = RESPONSE_RECV_PORT;
					if(peer_port == 0)
						peer_port = REQUEST_RECV_PORT; 
				}else {
					set_error("invalid role: %s", optarg); 
					return 1;
				}
				break;
      			case 'p':
        			port = atoi(optarg);
        			break;
			case 'P':
				peer_port = atoi(optarg);
				break;
		        case '?':
				if (optopt == 'r')
			  		set_error("Option -%c requires an argument.\n", optopt);
				else if (optopt == 'p')
			  		set_error("Option -%c requires an argument.\n", optopt);
				else if (optopt == 'P')
			  		set_error("Option -%c requires an argument.\n", optopt);
				else
			  		set_error("Unknown option character `%c'.\n", optopt);
				return 1;
		      	default:
				abort();
		}
	}

	for (index = optind; index < argc; index++){
		switch(role) {
			case DEAMON:
				if(register_event_script(argv[index])) {
					return 1;
				}
				break;
			case WAITFOR: 
    				if(register_poll_event(argv[index])) {
					return 1;
				}
		}
	}

	return 0;
}

// initialize listen sockets
int init_sockets(void) {
	struct sockaddr_in serveraddr;
	int optval;
	struct timeval read_timeout;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
    		set_error("ERROR opening socket");
		return 1;
	}
	optval = 1;
  	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, 
			(const void *)&optval , sizeof(int));
	
	memset(&serveraddr, 0, sizeof(serveraddr) );
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(port);
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

	read_timeout.tv_sec = 1;
	read_timeout.tv_usec = 0; 
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));

	if (bind(fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0 ) {
                set_error("Failed to create listen socket at port %d: %s\n", port, strerror(errno));	
        	return 1;
    	}
	return 0;
}

int send_reqresp(struct sockaddr_in sa, reqresp_body_t *request) {
	int sockfd;
	int resp = 0;

	/* socket: create the socket */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);

	resp = sendto(sockfd, (char *)request, sizeof(reqresp_body_t), 0, 
                                            (struct sockaddr *) &(sa), sizeof(sa));

	close(sockfd);	

	return resp;
}

// perform event request
int perform_event_request(char *event_id, char *error) {
	// search if the event exist 
	event_script_t *event_script;
	int status = 0;
	event_script = search_event_script(event_id);
	if (event_script == NULL) {
		set_error_s(error, "no script is registered for event \'%s\'", event_id);
		return 1;
	}
	// execute the event script
	status = system(event_script->script_path);
	if (status != 0) {
		set_error_s(error, "execution status failed (%d) for script \'%s\'", status,
 								event_script->script_path);
		return 1;
	}
	return 0;
}

// handle event response 
int handle_request_response(char *event_id, char *error) {
	event_task_t *event_task;
	// LOCK
	pthread_mutex_lock(&poll_event_list_lock);
	
	// find if event task exists
	event_task = search_poll_event(event_id);
	if (event_task == NULL) {
		set_error_s(error, "unknown poll event \'%s\' (or already finished)", event_id);
		pthread_mutex_unlock(&poll_event_list_lock);
		return 1;
	}

	// delete event task from the poll event list
	delete_poll_event(event_task);

	fprintf(stdout, "event \'%s\' is done\n", event_task->event_id);
	
	// free resource
	free(event_task->event_id);
	free(event_task);

	// UNLOCK 
	pthread_mutex_unlock(&poll_event_list_lock);
}

// poller polls for events to the corresponding services
static void *
poller(void *arg) {
	sigset_t set;	
	list_head_t *head;
	event_task_t *event_task; 
	reqresp_body_t request;
	int err;

	// block all signal
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);

	head = poll_event_list.start;
	do {
	   	// Lock poll event list lock
	   	pthread_mutex_lock(&poll_event_list_lock);
		if (head == NULL) {
			if (IS_EMPTY(&poll_event_list)) {
	   			pthread_mutex_unlock(&poll_event_list_lock);
				break;
			}else {
	   			pthread_mutex_unlock(&poll_event_list_lock);
				sleep(RESEND_TIME);
	   			pthread_mutex_lock(&poll_event_list_lock);
				head = poll_event_list.start;
				if (head == NULL) {
	   				pthread_mutex_unlock(&poll_event_list_lock);
					break;
				}
			}
		}
		event_task = CAST_OUT(head, event_task_t);
		
		// Get the event from poll
		head = head->next;

	   	// Unlock poll event lock
	   	pthread_mutex_unlock(&poll_event_list_lock);

		// set the request
		memset(&request, 0, sizeof(reqresp_body_t));
		request.is_req = (unsigned int)1;
		strcpy(request.event_id, event_task->event_id);
		
		// send request to the service deamon
		err = send_reqresp(event_task->sa, &request);
		if (err <= 0) {
			fprintf(stderr, "failed to sent event to %s error: %d\n", 
                                              inet_ntoa(event_task->sa.sin_addr), err);
		}
		fprintf(stdout, "send request for event \'%s\' to service %s:%u\n", event_task->event_id,
					inet_ntoa(event_task->sa.sin_addr), event_task->sa.sin_port);
	}while(!stop);

	// set stop so that the reader stops once poller is done
	stop = 1;	

	return NULL;
}

// reader listen for response otherwise request; on request it executes a 
// poll request in current context and send the response back to the requester;
// while in response it checkes if the event exist in poll event list and 
// delete the event from the list
static void *
reader(void *arg) {
	char buffer[sizeof(reqresp_body_t)];
	static char error_string[MAX_ERROR_LENGTH];
	int error = 0;
	int length;
	sigset_t set;
	struct sockaddr_in src_addr;
	socklen_t len = sizeof(src_addr);
	reqresp_body_t *reqresp;	

	// block all signal
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
	
	while(!stop) {
		error = 0;
		length = 0;
		memset(buffer, 0, sizeof(reqresp_body_t));
		do {
			length = recvfrom( fd, buffer + length, sizeof(reqresp_body_t) - length, 
								0, (struct sockaddr*)&src_addr, &len);
			if(length < 0) {
				error = errno;
				set_error_s(error_string, "failed to receive: %s\n", strerror(errno));
				break;
			}
			if(length < sizeof(reqresp_body_t)) {
				continue;
			}	
		}while(0);
		if (error) {
			if (error != ETIMEDOUT && error != EAGAIN)
				fprintf(stderr, error_string);
			continue;
		}
		// parse the request
		reqresp = (reqresp_body_t *)malloc(sizeof(reqresp_body_t));
		memcpy(reqresp, buffer, sizeof(reqresp_body_t));

		switch(reqresp->is_req) {
			// response
			case 0:
				// do not accept response as daemon
				if(role == DEAMON) {
					fprintf(stderr, "got response in deamon, check you ports\n");
					break;
				}
				fprintf(stdout, "received response for event \'%s\'\n", reqresp->event_id);
				error = handle_request_response(reqresp->event_id, error_string);
				if (error != 0) {
					fprintf(stderr, "failed to handle response: %s\n", error_string);
				}
				break;
			// request
			default:
				// do not accept request while waiting for
				if(role == WAITFOR) {
					fprintf(stderr, "got request while waiting for, check you ports\n");
					break;
				}
				fprintf(stdout, "received request for event \'%s\'\n", reqresp->event_id);
				// Perform event check request
				error = perform_event_request(reqresp->event_id, error_string);
				if (error != 0) {
					fprintf(stderr, "failed to perform request: %s\n", error_string);
					break;
				}
				// If event check is successful send response
				reqresp->is_req = 0;
				// change port to listen port
				src_addr.sin_port = htons(peer_port); 
				error = send_reqresp(src_addr, reqresp);	
				if (error <= 0) {
					fprintf(stderr, "failed to send response to %s error %d\n",
                                              				inet_ntoa(src_addr.sin_addr), error);
				}
				fprintf(stdout, "send request response for event \'%s\' to service %s:%u\n",
									reqresp->event_id,
									inet_ntoa(src_addr.sin_addr),
									src_addr.sin_port);
				break;
		}	
    	}
	close(fd);
	return NULL;
}

void term(int signum) {
	stop = 1;
}

int main(int argc, char *argv[]) {
	struct sigaction action;
	void *retval = NULL;

	// initialize
	if(init()) {
		fprintf(stderr, "Error: Failed to initialize: \n%7s\n", getError());
		exit(1); 
	}

	// parse arguments
	if(parse_argument(argc, argv)) {
		fprintf(stderr, "Error: Failed to parse argument: \n%7s\n", getError());
		cleanup();
		print_help(argv[0]);
		exit(1); 
	}

	// Create sockets
	if(init_sockets()) {
		fprintf(stderr, "Error: Failed to initialize the sockets: \n%7s\n", getError());
		cleanup();
		exit(1); 
	}

	// Start Reqreader
	pthread_create(&reader_thread, NULL, reader, NULL);
	if(role == WAITFOR) {
		pthread_create(&poller_thread, NULL, poller, NULL);
	}

	// Set signal handler
    	memset(&action, 0, sizeof(struct sigaction));
    	action.sa_handler = term;
    	sigaction(SIGTERM, &action, NULL);
    	sigaction(SIGINT, &action, NULL);

	// wait for thread to finish
	pthread_join(reader_thread, &retval);
	if(role == WAITFOR) {
		pthread_join(poller_thread, &retval);
	}
	// Clean Up
 	printf("stopping linker\n"); 
}
