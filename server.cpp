#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509_vfy.h>
#include <fcntl.h>
#include "constant.h"
#include "util.h"

using namespace std;
using uchar=unsigned char;

/*
* socket_id: if equal to -1 the user is not connected to the service
*/
struct user_info {
    string username;
    int socket_id; 
};




//Parameters of connection
const char *srv_ipv4 = "127.0.0.1";
const int srv_port = 4242;

//Handling mutual exclusion for accessing the user datastore
const char* sem_user_store_name = "/user_store";
void* create_shared_memory(ssize_t size);

//Shared memory for storing data of users
void* shmem = create_shared_memory(sizeof(user_info)*REGISTERED_USERS);

    
void* create_shared_memory(ssize_t size){
    int protection = PROT_READ | PROT_WRITE; //Processes can read/write the contents of the memory
    int visibility = MAP_SHARED | MAP_ANONYMOUS; //Memory pages are shared across processes
    return mmap(NULL, size, protection, visibility, -1, 0);
}


// ---------------------------------------------------------------------
// FUNCTIONS for accessing to the USER DATASTORE
// ---------------------------------------------------------------------
/**
 * @return -1: username not present, 0: successfully written socket data
 */

void sem_enter(sem_t* sem_id){
    vlog("sem_enter");
    if(sem_id == SEM_FAILED)
        errorHandler(SEM_OPEN_ERR);
    if(sem_wait(sem_id) < 0)
        errorHandler(SEM_WAIT_ERR);
}

void sem_exit(sem_t* sem_id){
    vlog("sem_exit");
    if(sem_post(sem_id) < 0)
        errorHandler(SEM_POST_ERR);
    if(sem_close(sem_id) < 0)
        errorHandler(SEM_CLOSE_ERR);
}

int set_user_socket(string username, int socket){

    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    sem_enter(sem_id);
    
    user_info* user_status = (user_info*)shmem;
    int found = -1;
    for(int i=0; i<REGISTERED_USERS; i++){
        if(user_status[i].username.compare(username) == 0){
            user_status[i].socket_id = socket;
            log("Set socket of " + username + " correctly");
            found = 0;
            break;
        }
    }

    sem_exit(sem_id);
    return found;
}


void print_user_data_store(){
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    sem_enter(sem_id);

    
    user_info* user_status = (user_info*)shmem;
    cout << "****** USER STATUS *******" << endl;
    for(int i=0; i<REGISTERED_USERS; i++){
        cout << i << ") " << user_status[i].username << " | " << user_status[i].socket_id << " | " << ((user_status[i].socket_id==-1)?"offline":"online") << endl;
    }

    sem_exit(sem_id);
}

/**
 * Need to free return value of this function 
 */
user_info* get_user_datastore_copy(){
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    sem_enter(sem_id);

    //Obtain a copy of the user datastore    
    user_info* user_status = (user_info*)malloc(REGISTERED_USERS*sizeof(user_info));
    if(!user_status)
        errorHandler(MALLOC_ERR);
    memcpy(user_status, shmem, REGISTERED_USERS*sizeof(user_info));

    sem_exit(sem_id);
    return user_status;
}

//Hardcoded content due to the fact that users are already registered
void initialize_user_info(user_info* user_status){
    user_status[0].username = "alice";
    user_status[0].socket_id = -1;
    user_status[1].username = "bob";
    user_status[1].socket_id = -1;
    user_status[2].username = "charlie";
    user_status[2].socket_id = -1;
    user_status[3].username = "dave";
    user_status[3].socket_id = -1;
    user_status[4].username = "ethan";
    user_status[4].socket_id = -1;
}

// ---------------------------------------------------------------------
// FUNCTIONS of HANDLING REPLIES FOR CLIENTS
// ---------------------------------------------------------------------


/**
 *  Handle the response to the client for the !chat command
 *  @return 0 in case of success, -1 in case of error
 */
int handle_chat_request(int comm_socket_id){
    log("CHAT opcode arrived");
    // Consuming the receiving buffer
    int user_id;
    int ret = recv(comm_socket_id, (void *)&user_id, sizeof(int), 0);

    if (ret < 0)
        errorHandler(REC_ERR);
    if (ret == 0)
        vlog("No message from the server");
    
    cout << "Request for chatting with user id " << user_id << " arrived " << endl;
    /*
    *    Work in progress
    */
    return 0;    
}

/**
 *  Handle the response to the client for the !users_online command
 *  @return 0 in case of success, -1 in case of error
 */
int handle_get_online_users(int comm_socket_id){
    log("USERS_ONLINE opcode arrived");
    int ret;
    
    /*
    *   Format of the message to send
    *     COMMAND_CODE | NUM_PAIRS | (USER_INDEX | LENGTH_USERNAME | USERNAME)*
    */

    unsigned char online_cmd = ONLINE_CMD;
    user_info* user_datastore_copy = get_user_datastore_copy();
    vlog("Obtained user datastore copy");

    //Need to calculate how much space to allocate and send (strings have variable length fields)
    int total_space_to_allocate = 5; //
    int online_users = 0; //also num_pairs
    int curr_position = 5; //Offset of the buffer
    
    for(int i=0; i<REGISTERED_USERS; i++){
        //Count only online users
        if(user_datastore_copy[i].socket_id != -1){
            total_space_to_allocate += user_datastore_copy[i].username.length() + 8;
            online_users++;
        }
    }

    vlog("Calculated reply size");
    
    //Copy various fields in the reply msg
    uchar* replyToSend = (uchar*)malloc(total_space_to_allocate);
    if(!replyToSend)
        errorHandler(MALLOC_ERR);
    uint32_t online_users_to_send = htonl(online_users);
    
    //Copy OPCODE and NUM_PAIRS
    memcpy(replyToSend, (void*)&online_cmd, sizeof(unsigned char));
    memcpy(replyToSend+1, (void*)&online_users_to_send, sizeof(int));
    
    for(int i=0; i<REGISTERED_USERS; i++){

        //Copy ID, USERNAME_LENGTH and USERNAME for online users
        if(user_datastore_copy[i].socket_id != -1){
            int curr_username_length = user_datastore_copy[i].username.length();
            uint32_t i_to_send = htonl(i);
            uint32_t curr_username_length_to_send = htonl(curr_username_length);
            
            memcpy(replyToSend + curr_position, (void*)&i_to_send, sizeof(int));
            memcpy(replyToSend + curr_position + 4, (void*)&curr_username_length_to_send, sizeof(int));
            memcpy(replyToSend + curr_position + 8, (void*)user_datastore_copy[i].username.c_str(), curr_username_length);

            curr_position = curr_position + 8 + curr_username_length;
        }
    }

    log("Total length of buffer to send: " + to_string(curr_position));
    BIO_dump_fp(stdout, (const char*)replyToSend, total_space_to_allocate);

    ret = send(comm_socket_id, replyToSend, total_space_to_allocate, 0);
    if(ret < 0 || ret!=total_space_to_allocate)
        errorHandler(SEND_ERR);
    
    free(replyToSend);
    free(user_datastore_copy);
    return 0;    
}



int main()
{
    //Create shared memory for mantaining info about users
    sem_unlink(sem_user_store_name); //Remove traces of usage for older execution 
    if(shmem == MAP_FAILED)
        errorHandler(GEN_ERR);
    user_info user_status[REGISTERED_USERS];
    initialize_user_info(user_status);
    memcpy(shmem, user_status, sizeof(user_info)*REGISTERED_USERS);
    set_user_socket("alice", 100); //to test the client

    int ret;
    int listen_socket_id, comm_socket_id;   //socket indexes
    struct sockaddr_in srv_addr, cl_addr;   //address informations
    //char send_buffer[1024];                 //buffer for sending replies
    pid_t pid;                              

    //Preparation of ip address struct
    memset(&srv_addr, 0, sizeof(srv_addr));
    listen_socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_id == -1)
        errorHandler(SOCK_ERR);

    //For avoiding annoying Address already in use error 
    int option = 1;
    setsockopt(listen_socket_id, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    //Configuration of server address
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(srv_port);
    inet_pton(AF_INET, srv_ipv4, &srv_addr.sin_addr);
    log("address struct preparation...");

    if (-1 == bind(listen_socket_id, (struct sockaddr *)&srv_addr, sizeof(srv_addr)))
        errorHandler(BIND_ERR);

    if (-1 == listen(listen_socket_id, SOCKET_QUEUE))
        errorHandler(LISTEN_ERR);

    unsigned int len = sizeof(cl_addr);
    log("Socket is listening...");

    while (true)
    {
        comm_socket_id = accept(listen_socket_id, (struct sockaddr *)&cl_addr, &len);
        pid = fork();

        if (pid == 0)
        {

            close(listen_socket_id);
            log("Connection established with client");

            /*
            * HANDLE AUTH and UPDATE USER DATA STORE
            */

            //Child process
            while (true)
            {
                uchar msgOpcode;
                
                //Get Opcode from the client
                ret = recv(comm_socket_id, (void *)&msgOpcode, sizeof(char), 0);

                if (ret < 0)
                    errorHandler(REC_ERR);
                if (ret == 0)
                    vlog("No message from the server");
           
                printf("The ocpode arrived is: %x\n",msgOpcode);

                //Demultiplexing of opcode
                switch (msgOpcode){
                    case CHAT_CMD:
                        ret = handle_chat_request(comm_socket_id);
                        if(ret<0)
                            errorHandler(GEN_ERR);
                        break;

                    case ONLINE_CMD:
                        ret = handle_get_online_users(comm_socket_id);
                        if(ret<0)
                            errorHandler(GEN_ERR);
                        break;
                    
                    default:
                        cout << "Command Not Valid" << endl;
                        break;
                }

            }
        }
        else if (pid == -1)
        {
            errorHandler(FORK_ERR);
        }

        close(comm_socket_id);
    }
}