#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_ERROR_LENGTH 1000

#define MAX_EVENT_ID_LENGTH 100
#define MAX_INCOMING_REQUEST_SIZE 512

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

#define RESET_NODE(head)          do{ head->next = NULL; head->prev = NULL; \
                                    }while(0)
#define ADD_NEXT(node, new)       do{ (new)->next = (node)->next; if ((new)->next != NULL) { \
                                      (new)->next->prev = new;} (new)->prev = node; \
                                      (node)->next = new; \
                                    }while(0)
#define ADD_PREV(node, new)       do{ (new)->prev = (node)->prev; if ((new)->prev != NULL) { \
                                      (new)->prev->next = new;} (new)->next = (node); \
                                      (node)->prev = new; \
                                    }while(0)
#define UNLINK_NODE(node)         do{ if((node)->next != NULL) { (node)->next->prev = (node)->prev; \
                                      } if((node)->prev != NULL) { \
                                      (node)->prev->next = (node)->next; } RESET_NODE((node)); \
                                    }while(0)
#define ADD_START(list, node)     do{ ADD_PREV((list)->start, node); (list)->start = node; \
                                      if((list)->end == NULL) { (list)->end = node; } \
                                    }while(0)
#define ADD_END(list, node)       do{ ADD_NEXT(((list)->end), (node)); (list)->end = node; \
                                      if((list)->start == NULL) { (list)->start = node; } \
                                    }while(0)

#define CAST_UP(ptr, type)        (type *)(ptr - (sizeof(type) - sizeof(list_head_t)))

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
typedef struct poll_request {
	char *event_id;
	struct sockaddr_in client_addr;
       	char *script_path;
	// node holds poll_reqyest into a list
	list_head_t node;
}poll_request_t;

/* request/response body */
typedef struct reqresp_body {
	unsigned int is_req;
	char event_id[MAX_EVENT_ID_LENGTH];
}reqresp_body_t;


/************************************* Linker Global Variables *******************************/

// Holds the list of event that is being waited for 
static list_t poll_event_list;
// Holds the list of event and script mapping
static list_t event_script_map_list; 
// Holds the list of event that need to be served 
static list_t request_queue;
// FFlag to stop threads
int stop = 0;
// port
static int port = 9999;
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
	return 0;
}

void print_help(char *exec_name) {
	printf("\n%s [opt] [event:script].. [service:event]..\n \
		\nopt: \n  \
		-h \t\t: print help\n \
		-r <role> \t: set role [deamon/waitfor] \n \
		-p <port> \t: listen/request port\n",
		exec_name
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
	ADD_END(&event_script_map_list, &es_node->node);
	if (event_script_map_list.start == NULL)
		event_script_map_list.start = &es_node->node;
		
	return 0;
}

// fimds a event_script object based on event id
event_script_t * search_event_script(char *event_id) {
	list_head_t *es_node;
	event_script_t *es_body = NULL;
	for(es_node = event_script_map_list.start; es_node != event_script_map_list.end;) {
		if (es_node == NULL) {
			break;
		}
		es_body = CAST_UP(es_node, event_script_t);
		if (strcmp(es_body->event_id, event_id) == 0) {
			return es_body;
		}
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
		es_body = CAST_UP(es_node, event_script_t);
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
    	he = gethostbyname(service);  
    	if(he == NULL) {
		set_error("service can't be resolved :%s\n", service);
		return 1;
	}

	
	// Create event script node
	et_node = (event_task_t *)malloc(sizeof(event_task_t));
	et_node->event_id = event;
	memcpy(&(et_node->sa.sin_addr), he->h_addr_list[0], he->h_length);
	et_node->sa.sin_family = AF_INET;
	et_node->sa.sin_port = htons(port);

	// Add node to event script list
	ADD_END(&poll_event_list, &et_node->node);
	if (poll_event_list.start == NULL)
		poll_event_list.start = &et_node->node;
		

	return 0;
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
		et_body = CAST_UP(et_node, event_task_t);
		free(et_body->event_id);
		UNLINK_NODE(et_node);
		free(et_body);
		et_node = et_next_node;
	}
}

// add a poll request to request queue
int add_poll_request(char *event_id, struct sockaddr_in *ca) {
	poll_request_t *request_node;
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
}

// clean the request queue
void clean_poll_request_queue(void) {
	list_head_t *poll_node;
	list_head_t *poll_next_node;
	poll_request_t *poll_request;
	
	for(poll_node = request_queue.start; poll_node != request_queue.end;) {
		if (poll_node == NULL) {
			break;
		}
		poll_next_node = poll_node->next;
		poll_request = CAST_UP(poll_node, poll_request_t);
		free(poll_request->event_id);
		UNLINK_NODE(poll_node);
		free(poll_request);
		poll_node = poll_next_node;
	}
}



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
	while ((c = getopt (argc, argv, "hr:p:")) != -1)
    		switch (c) {
			case 'h':
				print_help(argv[0]);
        			exit(0);
      			case 'r':
        			if(strcmp(optarg, "deamon") == 0) {
					printf("starting linker as deamon\n");
					role = DEAMON;
				} else if(strcmp(optarg, "deamon") == 0) {
					printf("starting linker to waitfor\n");
					role = WAITFOR;
				}else {
					set_error("invalid role: %s", optarg); 
					return 1;
				}
				break;
      			case 'p':
        			port = atoi(optarg);
        			break;
		        case '?':
				if (optopt == 'r')
			  		set_error("Option -%c requires an argument.\n", optopt);
				else if (optopt == 'p')
			  		set_error("Option -%c requires an argument.\n", optopt);
				else
			  		set_error("Unknown option character `%c'.\n", optopt);
				return 1;
		      	default:
				abort();
		}
	for (index = optind; index < argc; index++)
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

	return 0;
}

// initialize listen sockets
int init_sockets(void) {
	struct sockaddr_in serveraddr;
	memset( &serveraddr, 0, sizeof(serveraddr) );
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons( port );
	serveraddr.sin_addr.s_addr = htonl( INADDR_ANY );

	if ( bind(fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0 ) {
                set_error("Failed to create listen socket at port %d : %s\n", strerror(errno));	
        	return 1;
    	}
	return 0;
}

// poller polls for events to the corresponding services
static void *
poller(void *arg) {
	sigset_t set;	

	// block all signal
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
	
	return NULL;
}

// reader listen for response and request; on request it executes a poll request 
// in current context
static void *
reader(void *arg) {
	char buffer[MAX_INCOMING_REQUEST_SIZE];
	static char error_string[MAX_ERROR_LENGTH];
	int error = 0;
	int length;
	sigset_t set;
	
	// block all signal
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
	
	while(!stop) {
		do {
			length = recvfrom( fd, buffer, sizeof(buffer) - 1, 0, NULL, 0 );
			if ( length < 0 ) {
				set_error_s(error_string, "failed to receive: %s\n", strerror(errno));
				break;
			}
			buffer[length] = '\0';
			printf( "%d bytes: '%s'\n", length, buffer);
		}while(0);
		if (error) {
			
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

	// initialize
	if(init()) {
		fprintf(stderr, "Error: Failed to initialize: \n\t%s\n", getError());
		exit(1); 
	}

	// parse arguments
	if(parse_argument(argc, argv)) {
		fprintf(stderr, "Error: Failed to parse argument: \n\t%s\n", getError());
		cleanup();
		print_help(argv[0]);
		exit(1); 
	}

	// Create sockets
	if(init_sockets()) {
		fprintf(stderr, "Error: Failed to initialize the sockets: \n\t%s\n", getError());
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

	// 
} 
