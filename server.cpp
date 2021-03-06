#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <limits.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509_vfy.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <errno.h>
#include <fcntl.h>
#include "constant.h"
#include "util.h"
#include "crypto.h"

using namespace std;
using uchar=unsigned char;
typedef void (*sighandler_t)(int);



/*
* socket_id: if equal to -1 the user is not connected to the service
*/
struct user_info {
    string username;
    int socket_id; 
    int busy =0;
};


struct msg_to_relay{
    long type;
    char buffer[RELAY_MSG_SIZE];
};

//---------------- GLOBAL VARIABLES ------------------//
int client_user_id;
int comm_socket_id;
msg_to_relay relay_msg;

//Parameters of connection
const char *srv_ipv4 = "127.0.0.1";
const int srv_port = 4242;
void* server_privk;

uchar* session_key;
uint32_t session_key_len;

//Handling mutual exclusion for accessing the user datastore
const char* sem_user_store_name = "/user_store";
const char* message_queue_name = "/user_message_queue";

void* create_shared_memory(ssize_t size);

//Shared memory for storing data of users
void* shmem = create_shared_memory(sizeof(user_info)*REGISTERED_USERS);
int send_secure(int comm_socket_id, uchar* pt, uint pt_len);
int recv_secure(int comm_socket_id, unsigned char** plaintext);
    
void* create_shared_memory(ssize_t size){
    int protection = PROT_READ | PROT_WRITE; //Processes can read/write the contents of the memory
    int visibility = MAP_SHARED | MAP_ANONYMOUS; //Memory pages are shared across processes
    return mmap(NULL, size, protection, visibility, -1, 0);
}


// ---------------------------------------------------------------------
// FUNCTIONS for accessing to the USER DATASTORE
// ---------------------------------------------------------------------


/**
 * @brief prologue for granting mutual exclusion by using named semaphore sem_id
 * @return -1 in case of errors, 0 otherwise
 */
int sem_prologue(sem_t* sem_id){
    vlog("sem_enter");
    if(sem_id == nullptr){
        log("sem_prologue: nullptr found");    
        return -1;
    }
    if(sem_id == SEM_FAILED){
        log("SEM_FAILED");
        return -1;
    }
    if(-1 == sem_wait(sem_id)){
        log("ERROR on sem_wait");
        return -1;
    }
    return 0;
}

/**
 * @brief epilogue for granting mutual exclusion by using named semaphore sem_id
 * @return -1 in case of errors, 0 otherwise
 */
int sem_epilogue(sem_t* sem_id){
    vlog("sem_exit");
    if(sem_id == nullptr){
        log("sem_epilogue: nullptr found");    
        return -1;
    }
    if(-1 == sem_post(sem_id)){
        log("ERROR on sem_exit");
        return -1;
    }
    if(-1 == sem_close(sem_id)){
        log("ERROR on sem_close");
        return -1;
    }
    return 0;
}



/**
 * @brief test socket of communication in the user data store
 * @return return -1 in case the user is offline, -2 in case of errors, the socket_id otherwise
 */
int get_user_socket_by_user_id(int user_id){
    if(user_id < 0 || user_id >= REGISTERED_USERS){
        log("ERROR: Invalid user id");
        return -2;
    }
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return -2;
    }
    
    user_info* user_status = (user_info*)shmem;
    int socket_id = user_status[user_id].socket_id;

    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return -2;
    }

    return socket_id;
}

/**
 * @brief test if the user is buidi in a chat request
 * 
 * @param user_id 
 * @return int 0 if busy, 1 if free, -1 on error(s)
 */
int test_user_busy_by_user_id(int user_id){
    
    if(user_id < 0 || user_id >= REGISTERED_USERS){
        log("ERROR: Invalid user id");
        return -1;
    }
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return -1;
    }
    
    user_info* user_status = (user_info*)shmem;
    int ret=user_status[user_id].busy;
    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return -1;
    }
    return !ret;
}

/**
 * @brief set the user busy flag
 * 
 * @param user_id 
 * @param busy 
 * @return 1 on succes, -1 on error 
 */
int set_user_busy_by_user_id(int user_id, int busy){
    
    if(user_id < 0 || user_id >= REGISTERED_USERS){
        log("ERROR: Invalid user id");
        return -1;
    }
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return -1;
    }
    
    user_info* user_status = (user_info*)shmem;
    user_status[user_id].busy=busy;
    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return -1;
    }
    return 1;
}



/**
 * @brief test socket of communication in the user data store
 * @return return -1 in case of errors, 0 otherwise
 */
int set_user_socket(string username, int socket){
    if(socket < -1){ //Sanitization (-1 indicate that the user will be offline)
        log("SOCKET fd invalid"); //since file descriptors can have only values >= 0
        return -1;
    }
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return -1;
    }

    user_info* user_status = (user_info*)shmem;
    int found = 0;
    for(int i=0; i<REGISTERED_USERS; i++){
        if(user_status[i].username.compare(username) == 0){
            user_status[i].socket_id = socket;
            if(socket==-1)
                vlog("\n\n***** logout of client " +username+" *****\n\n");
            vlog("Set socket of " + username + " correctly");
            found = 1;
            break;
        }
    }
    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return -1;
    }
    return found;
}


/**
 * @brief prints content of user datastore for debugging purposes
 */
void print_user_data_store(){
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return;
    }
    
    user_info* user_status = (user_info*)shmem;
    cout << "\n\n****** USER STATUS *******\n\n" << endl;
    for(int i=0; i<REGISTERED_USERS; i++){
        cout << "[" << i << "] " << user_status[i].username << " | " << user_status[i].socket_id << " | " << " | "  << ((user_status[i].socket_id==-1)?"offline":"online") << endl;
    }
    cout << "\n\n**************************\n\n" << endl;

    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return;
    }
}

/**
 * @brief obtain a copy of the user datastore
 * @return the copy of the user datastore, nullptr in case of errors
 */
user_info* get_user_datastore_copy(){
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return nullptr;
    }
    int ret;
    char buf;
    //Obtain a copy of the user datastore    
    user_info* user_status = (user_info*)malloc(REGISTERED_USERS*sizeof(user_info));
    if(!user_status){
        log("ERROR on malloc");
        return nullptr;
    }
    memcpy(user_status, shmem, REGISTERED_USERS*sizeof(user_info));

    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return nullptr;
    }
    return user_status;
}


/**
 * @brief initialize content of user datastore which is used to simulate a database
 * @return 0 in case of errors, 1 in case of success
 */
int initialize_user_info(user_info* user_status){
    if(user_status == nullptr){
        return 0;
    }
    vector<string> usernames {"alice", "bob", "charlie", "dave", "ethan"};

    for(int i=0; i < REGISTERED_USERS; i++){
        user_status[i].username = usernames[i];
        user_status[i].socket_id = -1;
    }

    return 1;
}


int get_user_id_by_username(string username){
    vlog("Entering get id by username");
    if(username.empty()){
        log("INVALID usernam on get_user_id_by_username");
        return -1;
    }
    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return -1;
    }
    
    int ret = -1;
    user_info* user_status = (user_info*)shmem;
    for(int i=0; i<REGISTERED_USERS; i++){
        if(user_status[i].username.compare(username) == 0){
            vlog("Found username " + username + " in the datastore with user_id " + to_string(i));
            ret = i;
            break;
        }
    }
    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return -1;
    }
    return ret;
}



/**
 * @return username or empty string in case of errors
 */
string get_username_by_user_id(size_t id){
    vlog("get username by id");
    if(id >= REGISTERED_USERS){ 
        log(" ERR - User_id not present");
        errorHandler(GEN_ERR);
    }

    sem_t* sem_id= sem_open(sem_user_store_name, O_CREAT, 0600, 1);
    if(-1 == sem_prologue(sem_id)){
        log("ERROR on sem_prologue");
        return string();
    }

    user_info* user_status = (user_info*)shmem;
    string username = user_status[id].username;
    vlog("Obtained username of " + username);
    if(-1 == sem_epilogue(sem_id)){
        log("ERROR on sem_epilogue");
        return string();
    }
    return username;
}

/**
 *  @brief Removes traces of other execution due to the utilization of "named" data structures (semaphores and pipes) that can survive
 */
void prior_cleanup(){
    sem_unlink(sem_user_store_name); //Remove traces of usage for older execution  
    key_t key = ftok(message_queue_name, 65); 
    int msgid = msgget(key, 0666 | IPC_CREAT);
    msgctl(msgid, IPC_RMID, NULL);
    msgid = msgget(key, 0666 | IPC_CREAT);

    //Add more space to buffer
    struct msqid_ds buf;
    msgctl(msgid, IPC_STAT, &buf);
    buf.msg_qbytes = 600000;
    int ret = msgctl(msgid, IPC_SET, &buf);
    msgctl(msgid, IPC_STAT, &buf);    
    vlog("Message queue size: " + to_string(buf.msg_qbytes));
}

// ---------------------------------------------------------------------
// FUNCTIONS of INTER-PROCESS COMMUNICATION
// ---------------------------------------------------------------------

/** 
 *  Send message to message queue of to_user_id
 *  @return 0 in case of success, -1 in case of error
 */
int relay_write(uint to_user_id, msg_to_relay msg){
    if(to_user_id >= REGISTERED_USERS)
        return -1;
    
    vlog("Entering relay_write for " + to_string(to_user_id));
    msg.type = to_user_id + 1;
    
    key_t key = ftok(message_queue_name, 65); 
    vlog("Key of ftok returned is " + to_string(key));
    int msgid = msgget(key, 0666 | IPC_CREAT);
    vlog("msgid is " + to_string(msgid));

    return msgsnd(msgid, &msg, sizeof(msg_to_relay), 0);
}

/**
 * @brief read from message queue of user_id (blocking)
 * @return -1 if no message has been read otherwise return the bytes copied
 **/
int relay_read(int user_id, msg_to_relay& msg, bool blocking){
    uint time_remained;
    if(user_id >= REGISTERED_USERS || user_id < 0)
        return -1;
    
    if(blocking)
        time_remained = alarm(0);

    int ret = -1;
    vlog("relay_read of user_id " + to_string(user_id) + " [" + (blocking? "blocking": "non blocking") + "]");

    //Read from the message queue
    key_t key = ftok(message_queue_name, 65); 
    vlog("Key of ftok returned is " + to_string(key));
    //msg.type = 0;
    int msgid = msgget(key, 0666 | IPC_CREAT);
    vlog("msgid is " + to_string(msgid));
    
    ret = msgrcv(msgid, &msg, sizeof(msg), user_id+1, (blocking? 0: IPC_NOWAIT));
    if(ret == -1)
        vlog("read nothing");


    if(blocking){
        if(time_remained > 0)
            alarm(time_remained);
        else
            alarm(RELAY_CONTROL_TIME);
    }
    return ret;
}


// ---------------------------------------------------------------------
// FUNCTIONS of HANDLING SIGNALS
// ---------------------------------------------------------------------


/**
 * @brief Handler that handles the SIG_ALARM, this represents the fact that every REQUEST_CONTROL_TIME the client must control for chat request
 * @param sig 
 */
void signal_handler(int sig)
{
    vlog("signal handler");
    int ret;
    uint8_t opcode;
    int bytes_copied = relay_read(client_user_id, relay_msg, false);
    uint msg_len;

    if(bytes_copied > 0){
        opcode = relay_msg.buffer[0];
        log("Found request to relay with opcode: " + to_string(opcode));
        
        if(opcode == CHAT_CMD) {
            uint username_length, username_length_net;
            memcpy(&username_length_net, (void*)(relay_msg.buffer + 5), sizeof(int));
            username_length = ntohl(username_length_net);
            vlog("USERNAME LENGTH: " + to_string(username_length));
            
            if(username_length > UINT_MAX - 9 - PUBKEY_DEFAULT_SER){
                log("ERROR: unsigned wrap");
                return;
            }
            msg_len = 9 + username_length + PUBKEY_DEFAULT_SER;

            // Send reply of the peer to the client
            ret = send_secure(comm_socket_id, (uchar*)relay_msg.buffer, msg_len);
            if(ret == 0){
                log("ERROR on send_secure");
                close(comm_socket_id);
                exit(1);
            }       
            // log("Sent to client : ");    
            // BIO_dump_fp(stdout, (const char*)relay_msg.buffer, msg_len); 

        } else if(opcode == AUTH || opcode == CHAT_RESPONSE){
            memcpy(&msg_len, relay_msg.buffer + 1, sizeof(int)); //Added len field
            if(msg_len < 1){
                log("ERROR: msg_len < 1");
                close(comm_socket_id);
                exit(1);
            }

            uchar* msg_to_send = (uchar*)malloc(msg_len);
            if(!msg_to_send){
                log("ERROR on malloc");
                close(comm_socket_id);
                exit(1);
            }

            msg_to_send[0] = opcode;
            memcpy(msg_to_send + 1, relay_msg.buffer + 5, msg_len - 1);

            ret = send_secure(comm_socket_id, (uchar*)msg_to_send, msg_len);
            if(ret == 0){
                log("ERROR on send_secure");
                close(comm_socket_id);
                free(msg_to_send);
                exit(1);
            }       

            // log("Sent to client : ");    
            // BIO_dump_fp(stdout, (const char*)msg_to_send, msg_len);
            free(msg_to_send);
        } else if(opcode == STOP_CHAT || opcode == CHAT_NEG){
            msg_len = 5;

            // Send reply of the peer to the client
            ret = send_secure(comm_socket_id, (uchar*)relay_msg.buffer, msg_len);
            if(ret == 0){
                log("ERROR on send_secure");
                close(comm_socket_id);
                exit(1);
            }       

            // log("Sent to client : ");    
            // BIO_dump_fp(stdout, (const char*)relay_msg.buffer, msg_len);
        } else {
            log("OPCODE not recognized (" + to_string(opcode) + ")");
        }
    }
                

    alarm(RELAY_CONTROL_TIME);
    return;
}


// ---------------------------------------------------------------------
// FUNCTIONS of SECURITY
// ---------------------------------------------------------------------

uint32_t send_counter=0;

/**
 * @brief perform a an authenticad encryption and then a send operation
 * @param pt: pointer to plaintext without sequence number
 * @return 1 in case of success, 0 in case of error 
 */
int send_secure(int comm_socket_id, uchar* pt, uint pt_len){
    if(pt_len < 0 || comm_socket_id < 0){
        log("ERROR invalid parameters on send_secure");
        return 0;
    }

    int ret;
    uchar *tag, *iv, *ct, *aad;
    //alarm(0);

    uint aad_len;
    // log("Plaintext to send:");
    // BIO_dump_fp(stdout, (const char*)pt, pt_len);
    uint32_t header_len = sizeof(uint32_t)+IV_DEFAULT+TAG_DEFAULT;

    // adding sequence number
    uint32_t counter_n=htonl(send_counter);
    // cout <<" adding sequrnce number " << send_counter << endl;
    if(pt_len > UINT_MAX - sizeof(uint32_t)){
        log("ERROR: unsigned wrap");
        return 0;
    }

    uchar* pt_seq = (uchar*)malloc(pt_len+sizeof(uint32_t)); 
    memcpy(pt_seq , &counter_n, sizeof(uint32_t));
    memcpy(pt_seq+ sizeof(uint32_t), pt, pt_len);
    pt=pt_seq;
    pt_len+=sizeof(uint32_t);
    // log("Plaintext to send (with seq):");
    // BIO_dump_fp(stdout, (const char*)pt, pt_len);

    uint aad_ct_len_net = htonl(pt_len); //Since we use GCM ciphertext == plaintext
    uint ct_len = auth_enc_encrypt(pt, pt_len, (uchar*)&aad_ct_len_net, sizeof(uint), session_key, &tag, &iv, &ct);
    if(ct_len == 0){
        log("auth_enc_encrypt failed");
        return 0;
    }
    // log("ct_len: " + to_string(ct_len));
    if(ct_len > UINT_MAX - header_len){
        log("ERROR: unsigned wrap");
        return 0;
    } 
    uint msg_to_send_len = ct_len + header_len, bytes_copied = 0;
    uchar* msg_to_send = (uchar*)malloc(msg_to_send_len);
    if(!msg_to_send){
        // errorHandler(MALLOC_ERR);
        return 0;
    }

    // cout << aad_ct_len_net << " -> " << ntohl(aad_ct_len_net) << endl;
    memcpy(msg_to_send + bytes_copied, &aad_ct_len_net, sizeof(uint));
    bytes_copied += sizeof(uint);
    memcpy(msg_to_send + bytes_copied, iv, IV_DEFAULT);
    bytes_copied += IV_DEFAULT;
    memcpy(msg_to_send + bytes_copied, tag, TAG_DEFAULT);
    bytes_copied += TAG_DEFAULT;
    memcpy(msg_to_send + bytes_copied, ct, ct_len);
    bytes_copied += sizeof(uint);

    // log("Msg (authenticated and encrypted) to send, (copied " + to_string(bytes_copied) + " of " + to_string(msg_to_send_len) + "):");
    // BIO_dump_fp(stdout, (const char*)msg_to_send, msg_to_send_len);

    //-----------------------------------------------------------
    // Controllo encr/decr
    unsigned char* pt_test = NULL;
    int pt_len_test = auth_enc_decrypt(ct, ct_len, (uchar*)&aad_ct_len_net, sizeof(uint32_t), session_key, tag, iv, &pt_test);
    if(pt_len_test == 0){
        log("auth_enc_decrypt failed");
        return 0;
    }
    // log(" plaintext ");
    // BIO_dump_fp(stdout, (const char*)pt_test, pt_len_test);
    safe_free(pt, pt_len);
    //------------------------------------------------------
    ret = send(comm_socket_id, msg_to_send, msg_to_send_len, 0);
    if(ret <= 0 || ret != msg_to_send_len){
        errorHandler(SEND_ERR);
        safe_free(msg_to_send, msg_to_send_len);
        return 0;
    }
    send_counter++;
    if(send_counter == 0){
        log("ERROR: unsigned wrap on SEND COUNTER");
        return 0;
    }

    safe_free(msg_to_send, msg_to_send_len);
    //alarm(RELAY_CONTROL_TIME);
    return 1;
}


uint32_t receive_counter=0;

/**
 * @brief Receive in a secure way the messages sent by the server, decipher it and return the plaintext in the correspodent parameter. It
 * also control the sequence number
 * 
 * @param socket socket id
 * @param plaintext plaintext obtained by the decryption of the ciphertext
 * @return int plaintext length or -1 if error
 */
int recv_secure(int comm_socket_id, unsigned char** plaintext)
{
    // if(comm_socket_id < 0){
    //     log("INVALID parameters on recv_secure");
    //     return -1;
    // }

    vlog(" SECURE RECEIVE ");

    uint32_t header_len = sizeof(uint32_t)+IV_DEFAULT+TAG_DEFAULT; 
    uint32_t ct_len;
    unsigned char* ciphertext = NULL;
    uint32_t pt_len;
    int ret;
    //alarm(0);
    unsigned char* header = (unsigned char*)malloc(header_len);
    if(!header){
        cerr << " Error in malloc for header " << endl; 
        return -1;
    }
    unsigned char* iv = (unsigned char*)malloc(IV_DEFAULT);
    if(!iv){
        cerr << " Error in malloc for iv " << endl; 
        safe_free(header, header_len);
        return -1;
    }
    unsigned char* tag = (unsigned char*)malloc(TAG_DEFAULT);
    if(!tag){
        cerr << " Error in malloc for tag " << endl; 
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        return -1;
    }

    // Receive Header
    //cout << " DBG - Before recv " << endl;
    //BIO_dump_fp(stdout, (const char*)header, header_len);
    ret = recv(comm_socket_id, (void*)header, header_len, 0);
    if(ret <= 0 || ret != header_len){
        cerr << " Error in header reception " << ret << endl;
        close(comm_socket_id);
        BIO_dump_fp(stdout, (const char*)header, header_len);
        safe_free(tag, TAG_DEFAULT);
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        return -1;
    }
    // BIO_dump_fp(stdout, (const char*)header, header_len);

    // Open header
    memcpy((void*)&ct_len, header, sizeof(uint32_t));
    // log(" ct_len :");
    // BIO_dump_fp(stdout, (const char*)&ct_len, sizeof(uint32_t));

    memcpy(iv, header+sizeof(uint32_t), IV_DEFAULT);
    // log(" iv :");
    // BIO_dump_fp(stdout, (const char*)iv, IV_DEFAULT);

    memcpy(tag, header+sizeof(uint32_t)+IV_DEFAULT, TAG_DEFAULT);
    // log(" tag :");
    // BIO_dump_fp(stdout, (const char*)tag, TAG_DEFAULT);

    unsigned char* aad = (unsigned char*)malloc(sizeof(uint32_t));
    if(!aad){
        cerr << " Error in aad malloc " << endl;
        safe_free(tag, TAG_DEFAULT);
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        return -1;
    }
    memcpy(aad, header, sizeof(uint32_t));
    // log(" AAD : ");
    // BIO_dump_fp(stdout, (const char*)aad, sizeof(uint32_t));

    // Receive ciphertext
    // cout << " DBG - ct_len before ntohl is " << ct_len << endl;
    ct_len = ntohl(ct_len);
    // cout << " DBG - ct_len real is " << ct_len << endl;

    ciphertext = (unsigned char*)malloc(ct_len);
    if(!ciphertext){
        cerr << " Error in malloc for ciphertext " << endl;
        safe_free(tag, TAG_DEFAULT);
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        safe_free(aad, sizeof(uint32_t));
        return -1;
    }
    ret = recv(comm_socket_id, (void*)ciphertext, ct_len, 0);
    if(ret <= 0){
        cerr << " Error in AAD reception " << endl;
        safe_free(ciphertext, ct_len);
        safe_free(tag, TAG_DEFAULT);
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        safe_free(aad, sizeof(uint32_t));
        return -1;
    }
    // cout << " ciphertext is: " << endl;
    // BIO_dump_fp(stdout, (const char*)ciphertext, ct_len);

    // Decryption
    // cout<<"Session key:"<<endl;
    // BIO_dump_fp(stdout, (const char*) session_key, 32);
    pt_len = auth_enc_decrypt(ciphertext, ct_len, aad, sizeof(uint32_t), session_key, tag, iv, plaintext);
    if(pt_len == 0 || pt_len!=ct_len){
        cerr << " Error during decryption " << endl;
        safe_free(*plaintext, pt_len);
        safe_free(ciphertext, ct_len);
        safe_free(tag, TAG_DEFAULT);
        safe_free(header, header_len);
        safe_free(iv, IV_DEFAULT);
        safe_free(aad, sizeof(uint32_t));
        return -1;
    }
    // cout << " ciphertext is: " << endl;
    // BIO_dump_fp(stdout, (const char*)ciphertext, ct_len);
    // cout << " plaintext is " << endl;
    // BIO_dump_fp(stdout, (const char*)*plaintext, pt_len);
    safe_free(ciphertext, ct_len);
    safe_free(tag, TAG_DEFAULT);
    safe_free(header, header_len);
    safe_free(iv, IV_DEFAULT);
    safe_free(aad, sizeof(uint32_t));

    // check seq number
    uint32_t sequece_number = ntohl(*(uint32_t*) (*plaintext));
    // cout << " received sequence number " << sequece_number  << " aka " << *(uint32_t*) (*plaintext) << endl;
    // cout << " Expected sequence number " << receive_counter << endl;
    if(sequece_number<receive_counter){
        cerr << " Error: wrong seq number " << endl;
        safe_free(*plaintext, pt_len);
        return -1;
    }
    receive_counter=sequece_number+1;
    if(receive_counter == 0){
        log("ERROR: unsigned wrap on receive_counter");
        return -1;
    }

    //alarm(RELAY_CONTROL_TIME);
    return pt_len;
}


/**
 * @brief handle authentication with the client
 * @return user_id of the client or -1  in case of errors
 */
int handle_client_authentication(string pwd_for_keys){
    /*************************************************************
     * M1 - R1 and Username
     *************************************************************/
    int ret;
    uchar* R1 = (uchar*)malloc(NONCE_SIZE);
    if(!R1){
        errorHandler(MALLOC_ERR);
        return -1;
    }

    ret = recv(comm_socket_id, (void *)R1, NONCE_SIZE, 0);
    if (ret <= 0 || ret != NONCE_SIZE){
        errorHandler(REC_ERR);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }
    // log("M1 auth (0) Received R1: ");
    // BIO_dump_fp(stdout, (const char*)R1, NONCE_SIZE);

    uint32_t client_username_len;
    ret = recv(comm_socket_id, (void *)&client_username_len, sizeof(uint32_t), 0);
    if (ret <= 0 || ret != sizeof(uint32_t)){
        errorHandler(REC_ERR);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }
    client_username_len = ntohl(client_username_len);
    vlog("M1 auth (1) Received username size: " + to_string(client_username_len));

    char* username = (char*)malloc(client_username_len);
    if(!username){
        errorHandler(MALLOC_ERR);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }

    ret = recv(comm_socket_id, (void *)username, client_username_len, 0);
    if (ret <= 0 || ret != client_username_len){
        errorHandler(REC_ERR);
        safe_free((uchar*)username, client_username_len);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }
    string client_username(username);
    vlog("M1 auth (2) Received username: " + client_username);
    
    if(get_user_socket_by_user_id(get_user_id_by_username(username)) != -1){
        log("ERROR user already online");
        safe_free((uchar*)username, client_username_len);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }

    
    safe_free((uchar*)username, client_username_len);

    /*************************************************************
     * M2 - Send R2,pubkey_eph,signature,certificate
     *************************************************************/
    uchar* R2 = (uchar*)malloc(NONCE_SIZE);
    if(!R2){
        errorHandler(MALLOC_ERR);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }

    //Generate pair of ephermeral DH keys
    void* eph_privkey_s;
    uchar* eph_pubkey_s;
    uint eph_pubkey_s_len;
    ret = eph_key_generate(&eph_privkey_s, &eph_pubkey_s, &eph_pubkey_s_len);
    if(ret != 1){
        log("Error on EPH_KEY_GENERATE");
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }
    // log("M2 auth (1) pubkey: ");
    // BIO_dump_fp(stdout, (const char*)eph_pubkey_s, eph_pubkey_s_len);

    //Generate nuance R2
    ret = random_generate(NONCE_SIZE, R2);
    if(ret != 1){
        log("Error on random_generate");
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }

    // log("auth (2) R2: ");
    // BIO_dump_fp(stdout, (const char*)R2, NONCE_SIZE);

    //Get certificate of Server
    FILE* cert_file = fopen("certification/SecureCom_cert.pem", "rb");
    if(!cert_file){
        log("Error on opening cert file");
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }
    
    uchar* certificate_ser;
    uint certificate_len = serialize_certificate(cert_file, &certificate_ser);
    if(certificate_len == 0){
        log("Error on serialize certificate");
        fclose(cert_file);
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }
    // log("auth (3) certificate: ");
    // BIO_dump_fp(stdout, (const char*)certificate_ser, certificate_len);

    if(eph_pubkey_s_len > UINT_MAX - NONCE_SIZE*2){
        log("ERROR: unsigned wrap");
        return -1;
    }

    uint M2_to_sign_length = (NONCE_SIZE*2) + eph_pubkey_s_len, M2_signed_length;
    uchar* M2_signed;
    uchar* M2_to_sign = (uchar*)malloc(M2_to_sign_length);
    if(!M2_to_sign){
        log("Error on M2_to_sign");
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        fclose(cert_file);
        return -1;
    }

    memcpy(M2_to_sign, R1, NONCE_SIZE);
    memcpy((void*)(M2_to_sign + NONCE_SIZE), R2, NONCE_SIZE);
    memcpy((void*)(M2_to_sign + (2*NONCE_SIZE)), eph_pubkey_s, eph_pubkey_s_len);
    // log("auth (4) M2_to_sign: ");
    // BIO_dump_fp(stdout, (const char*)M2_to_sign, M2_to_sign_length);


    ret = sign_document(M2_to_sign, M2_to_sign_length, server_privk,&M2_signed, &M2_signed_length);
    if(ret != 1){
        log("Error on signing part on M2");
        safe_free(M2_to_sign, M2_to_sign_length);
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        fclose(cert_file);
        return -1;
    }
    //Send M2 part by part
    if(eph_pubkey_s_len > UINT_MAX - 3*sizeof(uint) - M2_signed_length){
        log("ERROR unsigned_wrap");
        return -1;
    }

    if(certificate_len > UINT_MAX - 3*sizeof(uint) - eph_pubkey_s_len - M2_signed_length){
        log("ERROR unsigned_wrap");
        return -1;
    }

    uint M2_size = NONCE_SIZE + 3*sizeof(uint) + eph_pubkey_s_len + M2_signed_length + certificate_len; 
    uint offset = 0;
    uchar* M2 = (uchar*)malloc(M2_size);
    if(!M2){
        log("ERROR on malloc");
         safe_free(M2_to_sign, M2_to_sign_length);
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }
    uint eph_pubkey_s_len_net = htonl(eph_pubkey_s_len);
    uint M2_signed_length_net = htonl(M2_signed_length);
    uint certificate_len_net = htonl(certificate_len);
    vlog("Copying");
    memcpy((void*)(M2 + offset), R2, NONCE_SIZE);
    offset += NONCE_SIZE;
    memcpy((void*)(M2 + offset), &eph_pubkey_s_len_net, sizeof(uint));
    offset += sizeof(uint);
    memcpy((void*)(M2 + offset), eph_pubkey_s, eph_pubkey_s_len);
    offset += eph_pubkey_s_len;
    memcpy((void*)(M2 + offset), &M2_signed_length_net ,sizeof(uint));
    offset += sizeof(uint);
    memcpy((void*)(M2 + offset), M2_signed,M2_signed_length);
    offset += M2_signed_length;
    memcpy((void*)(M2 + offset), &certificate_len_net ,sizeof(uint));
    offset += sizeof(uint);
    memcpy((void*)(M2 + offset), certificate_ser, certificate_len);
    offset += certificate_len;
    
    vlog("M2 size: " + to_string(M2_size));
    
    ret = send(comm_socket_id, M2, M2_size, 0);
    if(ret < M2_size){
        errorHandler(SEND_ERR);
        safe_free(M2_to_sign, M2_to_sign_length);
        safe_free(R1, NONCE_SIZE);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_s, eph_pubkey_s_len);
        return -1;
    }
    // log("M2 sent");
    // BIO_dump_fp(stdout, (const char*)M2, offset);

        
    safe_free(M2, M2_size);
    safe_free(M2_to_sign, M2_to_sign_length);
    safe_free(R1, NONCE_SIZE);
    safe_free(eph_pubkey_s, eph_pubkey_s_len);
    

    /*************************************************************
     * M3 - client_pubkey and signing of pubkey and R2
     *************************************************************/
    uint32_t eph_pubkey_c_len;
    ret = recv(comm_socket_id, &eph_pubkey_c_len, sizeof(uint32_t), 0);
    if(ret <= 0 || ret != sizeof(uint32_t)){
        errorHandler(REC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        return -1;
    }
    
    eph_pubkey_c_len = ntohl(eph_pubkey_c_len);
    //eph_pubkey_c_len =178;
    vlog("M3 auth (1) pubkey_c_len: "+ to_string(eph_pubkey_c_len));

    uchar* eph_pubkey_c = (uchar*)malloc(eph_pubkey_c_len);
    if(!eph_pubkey_c ){
        errorHandler(MALLOC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        return -1;
    }

    ret = recv(comm_socket_id, eph_pubkey_c, eph_pubkey_c_len, 0);
    if(ret <= 0 || ret != eph_pubkey_c_len){
        errorHandler(REC_ERR);
        free(R2);
        free(eph_pubkey_c);
        return -1;
    }
    // log("M3 auth (2) pubkey_c:");
    // BIO_dump_fp(stdout, (const char*)eph_pubkey_c, eph_pubkey_c_len);

    uint32_t m3_signature_len;
    ret = recv(comm_socket_id, &m3_signature_len, sizeof(uint32_t), 0);
    if(ret <= 0){
        errorHandler(REC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        return -1;
    }
    m3_signature_len = ntohl(m3_signature_len);
    vlog("M3 auth (3) m3_signature_len: "+ to_string(m3_signature_len));

    uchar* M3_signed = (uchar*)malloc(m3_signature_len);
    if(!M3_signed){
        errorHandler(MALLOC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        return -1;
    }
    ret = recv(comm_socket_id, M3_signed, m3_signature_len, 0);
    if(ret <= 0){
        errorHandler(REC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        return -1;
    }

    // log("auth (4) M3 signed:");
    // BIO_dump_fp(stdout, (const char*)M3_signed, m3_signature_len);

    string pubkey_of_client_path = "certification/" + client_username + "_pubkey.pem";
    FILE* pubkey_of_client = fopen(pubkey_of_client_path.c_str(), "rb");
    if(!pubkey_of_client){
        log("Unable to open pubkey of client");
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        return -1;
    }

    if(eph_pubkey_c_len > UINT_MAX - NONCE_SIZE){
        log("ERROR unsigned wrap");
        return -1;
    }
    uint m3_document_size = eph_pubkey_c_len + NONCE_SIZE;
    uchar* m3_document = (uchar*)malloc(m3_document_size);
    if(!m3_document){
        errorHandler(MALLOC_ERR);
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        fclose(pubkey_of_client);
        return -1;
    }

    memcpy(m3_document, eph_pubkey_c,eph_pubkey_c_len );
    memcpy(m3_document+eph_pubkey_c_len, R2, NONCE_SIZE);
    vlog("auth (5) M3, verifying sign");
    ret = verify_sign_pubkey(M3_signed, m3_signature_len,m3_document,m3_document_size, pubkey_of_client);
    if(ret == 0){
        log("Failed sign verification on M3");
        safe_free(R2, NONCE_SIZE);
        safe_free_privkey(eph_privkey_s);
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        fclose(pubkey_of_client);
        return -1;
    }
    fclose(pubkey_of_client);
    uchar* shared_seceret;
    uint shared_seceret_len;
    vlog("auth (6) Creating session key");
    shared_seceret_len = derive_secret(eph_privkey_s, eph_pubkey_c, eph_pubkey_c_len, &shared_seceret);
    if(shared_seceret_len == 0){
        log("Failed derive secret");
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        return -1;    
    }
    // log("Shared Secret!");
    // BIO_dump_fp(stdout, (const char*) shared_seceret, shared_seceret_len); 
    session_key_len=default_digest(shared_seceret, shared_seceret_len, &session_key);
    if(session_key_len == 0){
        log("Failed digest computation of the secret");
        safe_free(eph_pubkey_c, eph_pubkey_c_len);
        safe_free(M3_signed, m3_signature_len);
        safe_free(shared_seceret, shared_seceret_len);
        return -1;    
    }
    // log("Session key generated!");
    // BIO_dump_fp(stdout, (const char*) session_key, session_key_len);
    safe_free(eph_pubkey_c, eph_pubkey_c_len);
    safe_free(M3_signed, m3_signature_len);
    safe_free(shared_seceret, shared_seceret_len);
    //Send user id of the client 
    int client_user_id = get_user_id_by_username(client_username);
    int client_user_id_net = htonl(client_user_id);
    vlog("Found username in the datastore with user_id " + to_string(client_user_id_net));

    //Set that user is online
    ret = set_user_socket(client_username, comm_socket_id);
    if(ret == -1){
        log("ERROR on set_user_socket");
        safe_free((uchar*)username, client_username_len);
        safe_free(R1, NONCE_SIZE);
        return -1;
    }

    //SEND User id
    uchar* userID_msg=(uchar*)malloc(5);
    *userID_msg=USRID;
    memcpy(userID_msg+1, &client_user_id_net,4);
    
    ret = send_secure(comm_socket_id, userID_msg, 5);
    if(ret == 0){
        log("Error on send secure");
        return -1;
    }

    
    //Check if present in the user_datastore
    free(username);
    return get_user_id_by_username(client_username);
}




// ---------------------------------------------------------------------
// FUNCTIONS of HANDLING REPLIES FOR CLIENTS
// ---------------------------------------------------------------------

/**
 *  Handle the response to the client for the !users_online command
 *  @return 0 in case of success, -1 in case of error
 */
int handle_get_online_users(int comm_socket_id, uchar* plaintext){
    if(comm_socket_id < 0 || plaintext == nullptr){
        log("Invalid input parameters on handle_get_online_users");
        return -1;
    }

    log("\n*** USERS_ONLINE ***\n");
    int ret;
    uint offset_reply = 0; 
    unsigned char online_cmd = ONLINE_CMD;
    user_info* user_datastore_copy = get_user_datastore_copy();

    //Need to calculate how much space to allocate and send (limited users and username sizes in this context, can't overflow those values)
    int total_space_to_allocate = 9; 
    int online_users = 0; //also num_pairs
    
    for(int i=0; i<REGISTERED_USERS; i++){
        //Count only online users
        if(user_datastore_copy[i].socket_id != -1){
            total_space_to_allocate += user_datastore_copy[i].username.length() + 8;
            online_users++;
        }
    }

    vlog("Calculated reply size (pt): " + to_string(total_space_to_allocate));
    
    //Copy various fields in the reply msg
    uchar* replyToSend = (uchar*)malloc(total_space_to_allocate);
    if(!replyToSend)
        errorHandler(MALLOC_ERR);
    uint32_t online_users_to_send = htonl(online_users);
    
    //Copy OPCODE and NUM_PAIRS
    memcpy(replyToSend+offset_reply, (void*)&online_cmd, sizeof(uchar));
    offset_reply += sizeof(uchar);
    memcpy(replyToSend+offset_reply, (void*)&online_users_to_send, sizeof(int));
    offset_reply += sizeof(int);

    for(int i=0; i<REGISTERED_USERS; i++){

        //Copy ID, USERNAME_LENGTH and USERNAME for online users
        if(user_datastore_copy[i].socket_id != -1){
            int curr_username_length = user_datastore_copy[i].username.length();
            uint32_t i_to_send = htonl(i);
            uint32_t curr_username_length_to_send = htonl(curr_username_length);
            
            memcpy(replyToSend + offset_reply, (void*)&i_to_send, sizeof(int));
            offset_reply += sizeof(int);
            memcpy(replyToSend + offset_reply, (void*)&curr_username_length_to_send, sizeof(int));
            offset_reply += sizeof(int);
            memcpy(replyToSend + offset_reply, (void*)user_datastore_copy[i].username.c_str(), curr_username_length);
            offset_reply += curr_username_length;
        }
    }
    vlog("Offset reply: " + to_string(offset_reply));
    ret = send_secure(comm_socket_id, (uchar*)replyToSend, offset_reply);
    if(ret == 0){
        safe_free(replyToSend, total_space_to_allocate);
        safe_free((uchar*)user_datastore_copy, REGISTERED_USERS*sizeof(user_info));
        errorHandler(SEND_ERR);
        return -1;
    }

    // log("Sent to client (pt): ");
    // BIO_dump_fp(stdout, (const char*)replyToSend, ret);
        
    safe_free(replyToSend, total_space_to_allocate);
    safe_free((uchar*)user_datastore_copy, REGISTERED_USERS*sizeof(user_info));
    return 0;    
}



/**u
 *  @brief Handle the response to the client for the !chat command
 *  @return 0 in case of success, -1 in case of error
 */
int handle_chat_request(int comm_socket_id, int client_user_id, msg_to_relay& relay_msg, uchar* plaintext, uint plain_len){
    if(comm_socket_id < 0 || client_user_id < 0 || client_user_id >= REGISTERED_USERS || plaintext == nullptr){
        log("Invalid input parameters on handle_chat_request");
        return -1;
    }
    if(!set_user_busy_by_user_id(client_user_id, 1)){
        log("ERROR: setting user busy while requesting to chat \n");
        return -1;
    }
    if(plain_len != 9){
        log("ERROR on length of plaintext");
        return -1;
    }

    log("\n*** CHAT_REQUEST ***\n");
    uint offset_plaintext = 5; //From where data is good to read 
    uint offset_relay = 0;
    int ret;

    int peer_user_id_net;
    memcpy(&peer_user_id_net,(const void*)(plaintext + offset_plaintext),sizeof(int));
    offset_plaintext += sizeof(int);
    int peer_user_id = ntohl(peer_user_id_net);
    if(peer_user_id < 0 || peer_user_id >= REGISTERED_USERS){
        log("ERROR: invalid value of peer user id in handle_chat_request");
        return -1;
    }

    unsigned char chat_cmd = CHAT_CMD;
    string client_username = get_username_by_user_id(client_user_id);
    if(client_username.empty()){
        log("ERROR on get_username_by_user_id");
        return -1;
    }

    int client_username_length = client_username.length();
    uint32_t client_username_length_net = htonl(client_username_length);
    uint32_t client_user_id_net = htonl(client_user_id);
    const char* username = client_username.c_str();
    vlog(username);
    vlog("Request for chatting with user id " +  to_string(peer_user_id) + " arrived ");

    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&chat_cmd, 1);
    offset_relay += sizeof(uchar);
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&client_user_id_net, sizeof(int));
    offset_relay += sizeof(int);
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&client_username_length_net, sizeof(int));
    offset_relay += sizeof(int);
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)username, client_username_length);
    offset_relay += client_username_length;

    string pubkey_of_client_path = "certification/" + client_username + "_pubkey.pem";
    vlog("Opening " + pubkey_of_client_path);
    FILE* pubkey_of_client_file = fopen(pubkey_of_client_path.c_str(), "rb");
    if(!pubkey_of_client_file){
        log("Unable to open pubkey of client");
        return -1;
    }
    uchar* pubkey_client_ser;
    int pubkey_client_ser_len = serialize_pubkey_from_file(pubkey_of_client_file, &pubkey_client_ser);
    // log("Pubkey ser len : " + to_string(pubkey_client_ser_len) + "(default: " + to_string(PUBKEY_DEFAULT_SER) + "), pubkey_ser:");
    // BIO_dump_fp(stdout, (const char*)pubkey_client_ser, pubkey_client_ser_len);

    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)pubkey_client_ser, pubkey_client_ser_len);
    offset_relay += pubkey_client_ser_len;
    
    uint8_t opcode = relay_msg.buffer[0];

    int final_response_len;
    if(opcode == CHAT_NEG)
        final_response_len = 5;
    else
        final_response_len = 5 + PUBKEY_DEFAULT_SER;
    vlog("Relaying ");
    // BIO_dump_fp(stdout, relay_msg.buffer, offset_relay);    
    vlog("Handle chat request (2)");

    //Handle case user is offline
    if(get_user_socket_by_user_id(peer_user_id) == -1 || !test_user_busy_by_user_id(peer_user_id)){
        log("User: " + to_string(peer_user_id) + "is offline or busy. Sending CHAT_NEG");
        uchar chat_cmd = CHAT_NEG;
        offset_relay = 0; 
        memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&chat_cmd, 1);
        offset_relay += sizeof(uchar);
        memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&peer_user_id_net, sizeof(int));
        offset_relay += sizeof(int);
        relay_write(client_user_id, relay_msg);
        return 0;
    }

    relay_write(peer_user_id, relay_msg);

    //Wait for response to the own named message queue (blocking)
    vlog("Handle chat request (3)");
    relay_read(client_user_id, relay_msg, true); 
    memcpy((void*)(relay_msg.buffer + 1), (void*)&peer_user_id, sizeof(int));    

    
    vlog("Handle chat request (4)");
    // Send reply of the peer to the client
    ret = send_secure(comm_socket_id, (uchar*)relay_msg.buffer, final_response_len);
    if(ret == 0){
        errorHandler(SEND_ERR);
        return -1;
    }
    if(!set_user_busy_by_user_id(client_user_id, 0)){
        log("ERROR: setting user busy while e ending requesting to chat \n");
        return -1;
    }
    return 0;    
}


/**
 * @brief handle CHAT_POS and CHAT_NEG commands
 * @return -1 in case of errors, 0 in case of success
 */
int handle_chat_pos_neg(uchar* plaintext, uint8_t opcode, uint plain_len){
    if(plaintext == nullptr){
        log("ERROR invalid parameter on handle_chat_pos_neg");
        return -1;
    }
    if(plain_len != 9){
        log("INVALID plain_len");
        return -1;
    }

    if(opcode == CHAT_POS)
        log("\n\n*** CHAT_POS ***\n");
    else if(opcode == CHAT_NEG)
        log("\n\n*** CHAT_NEG ***\n");
    else if(opcode == STOP_CHAT)
        log("\n\n*** STOP_CHAT ***\n");
    else{
        log("invalid opcode on handle_chat_pos_neg");
        return -1;
    }
        
    
    uint offset_plaintext = 5;
    uint offset_relay = 0;
    int peer_user_id_net = *(int*)(plaintext + offset_plaintext);
    offset_plaintext += sizeof(int);
    // int peer_user_id;
    // int peer_user_id_net;
    // int ret = recv(comm_socket_id, (void *)&peer_user_id_net, sizeof(int), 0);
    // if (ret < 0)
    //     errorHandler(REC_ERR);
    // if (ret == 0){
    //     vlog("No message from the server");
    //     exit(1);
    // }

    int peer_user_id = ntohl(peer_user_id_net);
    if(peer_user_id < 0 || peer_user_id >= REGISTERED_USERS){
        log("INVALID peer_user_id on handle_chat_pos_neg");
        return -1;
    }
    vlog("Command to send for user_id " +  to_string(peer_user_id) + " arrived ");
    

    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&opcode, sizeof(uchar));
    offset_relay += sizeof(uchar);
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&peer_user_id_net, sizeof(int));
    offset_relay += sizeof(int);
    if(opcode == CHAT_POS){
        string client_username = get_username_by_user_id(client_user_id);
        if(client_username.empty()){
            log("ERROR on get_username_by_user_id");
            return -1;
        }
        string pubkey_of_client_path = "certification/" + client_username + "_pubkey.pem";
        vlog("Opening " + pubkey_of_client_path);
        FILE* pubkey_of_client_file = fopen(pubkey_of_client_path.c_str(), "rb");
        if(!pubkey_of_client_file){
            log("Unable to open pubkey of client");
            return -1;
        }
        
        //Adding pubkey
        uchar* pubkey_client_ser;
        int pubkey_client_ser_len = serialize_pubkey_from_file(pubkey_of_client_file, &pubkey_client_ser);
        vlog("Pubkey ser len : " + to_string(pubkey_client_ser_len) + "(default: " + to_string(PUBKEY_DEFAULT_SER) + "), pubkey_client_ser:");
        // BIO_dump_fp(stdout, (const char*)pubkey_client_ser, pubkey_client_ser_len);

        // memcpy((void*)(relay_msg.buffer + offset_relay), &pubkey_client_ser_len, sizeof(int));
        // offset_relay += sizeof(int);
        memcpy((void*)(relay_msg.buffer + offset_relay), (void*)pubkey_client_ser, PUBKEY_DEFAULT_SER);
        offset_relay += pubkey_client_ser_len;
    }
    
    vlog("Relaying: ");
    // BIO_dump_fp(stdout, relay_msg.buffer, offset_relay);
    // if(get_user_socket_by_user_id(peer_user_id) == -1){
    //     log("User: " + to_string(peer_user_id) + "is offline. Sending CHAT_NEG");
    //     uchar chat_cmd = CHAT_NEG;
    //     offset_relay = 0; 
    //     memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&chat_cmd, 1);
    //     offset_relay += sizeof(uchar);
    //     memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&peer_user_id_net, sizeof(int));
    //     offset_relay += sizeof(int);
    //     relay_write(peer_user_id, relay_msg);
    //     return 0;
    // }
    relay_write(peer_user_id, relay_msg);
    return 0;
}


/**
 * @brief handles AUTH and CHAT_RESPONSE commands
 * @return -1 in case of errors, 0 instead
 */
int handle_auth_and_msg(uchar* plaintext, uint8_t opcode, int plaintext_len){
    if(opcode == AUTH)
        log("\n *** AUTH (" + to_string(opcode) + ") ***\n");
    else if(opcode == CHAT_RESPONSE) 
        log("\n *** CHAT_RESPONSE ***\n");
    else{
        log("invalid opcode on handle_chat_pos_neg");
        return -1;
    }

    if(plaintext_len < 5 || plaintext_len > RELAY_MSG_SIZE || plaintext == nullptr){
        log("INVALID plaintext_len on handle_auth_and_msg");
        return -1;
    }

    uint offset_plaintext = 5;
    uint offset_relay = 0;
    int plain_len_without_seq = plaintext_len - 4;
    int peer_user_id_net = *(int*)(plaintext + offset_plaintext);
    offset_plaintext += sizeof(int);
    int peer_user_id = ntohl(peer_user_id_net);
    
    vlog("Command to send for user_id " +  to_string(peer_user_id) + " arrived ");
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&opcode, sizeof(uint8_t));
    offset_relay += sizeof(uint8_t);
    //Add length of msg in between
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&plain_len_without_seq, sizeof(int));
    offset_relay += sizeof(int);
    memcpy((void*)(relay_msg.buffer + offset_relay), (void*)(plaintext + 5), plaintext_len - 5);
    offset_relay += (plaintext_len - 5);
    vlog("plain_len_without_seq: " + to_string(plain_len_without_seq));
    vlog("Relaying: ");
    // BIO_dump_fp(stdout, relay_msg.buffer, offset_relay);

    //Control if user is offline
    if(get_user_socket_by_user_id(peer_user_id) == -1){
        log("User: " + to_string(peer_user_id) + "is offline. Sending STOP_CHAT");
        uchar chat_cmd = STOP_CHAT;
        offset_relay = 0; 
        memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&chat_cmd, 1);
        offset_relay += sizeof(uchar);
        memcpy((void*)(relay_msg.buffer + offset_relay), (void*)&peer_user_id_net, sizeof(int));
        offset_relay += sizeof(int);
        relay_write(client_user_id, relay_msg);
        return 0;
    }
    return relay_write(peer_user_id, relay_msg);
}



int main(){
    //Create shared memory for mantaining info about users
    int ret=system("sudo sysctl -w kernel.msgmni=16384 kernel.msgmax=120000 kernel.msgmnb=600000");
    if(ret<0){
        log("failed to initialize kernel variables for message queue");
        return 0;
    }
    prior_cleanup();
    if(shmem == MAP_FAILED){
        log("MMAP failed");
        return 0;
    }
    user_info user_status[REGISTERED_USERS];
    ret = initialize_user_info(user_status);
    if(ret == 0){
        log("ERROR on initialize_user_info");
        return 0;
    }
    memcpy(shmem, user_status, sizeof(user_info)*REGISTERED_USERS);
    
    int listen_socket_id;                   //socket indexes
    struct sockaddr_in srv_addr, cl_addr;   //address informations
    pid_t pid;                              
    string password_for_keys;               
    uchar msgOpcode;                        //where is received the opcode of the message
    uchar* plaintext;                       //buffer to store the plaintext
    int plain_len;

    // WE MAY WANT TO DISABLE ECHO
    cout << "Enter the password that will be used for reading the keys: ";
    FILE* server_key = fopen("certification/SecureCom_prvkey.pem", "rb");
    server_privk=read_privkey(server_key, NULL);
    if(!server_privk){
        cerr << "Wrong key!";
        exit(1);
    }

    //Preparation of ip address struct
    memset(&srv_addr, 0, sizeof(srv_addr));
    listen_socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_id == -1){
        log("ERROR on socket");
        return 0;
    }

    //For avoiding annoying address already in use error 
    int option = 1;
    setsockopt(listen_socket_id, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    //Configuration of server address
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(srv_port);
    if(-1 == inet_pton(AF_INET, srv_ipv4, &srv_addr.sin_addr)){
        log("ERROR on inet_pton: ");
        perror(strerror(errno));
        return 0;
    }

    if (-1 == bind(listen_socket_id, (struct sockaddr *)&srv_addr, sizeof(srv_addr))){
        log("ERROR on bind: ");
        perror(strerror(errno));
        return 0;
    }
        
    if (-1 == listen(listen_socket_id, SOCKET_QUEUE)){
        log("ERROR on listen: ");    
        perror(strerror(errno));
        return 0;
    }
        
    unsigned int len = sizeof(cl_addr);
    log("Socket is listening...");

    while (true){
        comm_socket_id = accept(listen_socket_id, (struct sockaddr *)&cl_addr, &len);
        if(comm_socket_id == -1){
            log("ERROR on accept");
            return 0;
        }

        pid = fork();

        if (pid == 0){
            close(listen_socket_id);
            log("Connection established with client");

            //Manage authentication
            client_user_id = handle_client_authentication(password_for_keys);
            if(client_user_id == -1){
                errorHandler(AUTHENTICATION_ERR);
                log("Errore di autenticazione");
                return -1;
            }
            string client_username = get_username_by_user_id(client_user_id);
            if(client_username.empty()){
                log("ERROR on get_username_by_user_id");
                return -1;
            }
            
            log("--- AUTHENTICATION COMPLETED WITH user: " + client_username);
            // Every REQUEST_CONTROL_TIME seconds a signal is issued to control if the server has sent
            // a chat request originated from another clientS 
            if(SIG_ERR == signal(SIGALRM, signal_handler)){
                log("ERROR on signal");
                safe_free(session_key, session_key_len);
                set_user_socket(get_username_by_user_id(client_user_id), -1);
                close(comm_socket_id);
                return 0;
            }

            alarm(RELAY_CONTROL_TIME);

            //Child process
            while (true){
                
                plain_len = recv_secure(comm_socket_id, &plaintext);

                if(plain_len <= 4){
                    log("ERROR on recv_secure (at least seq num and opcode should be read) (" + to_string(plain_len) + ")");
                    safe_free(session_key, session_key_len);
                    set_user_socket(get_username_by_user_id(client_user_id), -1);
                    close(comm_socket_id);
                    return -1;
                }
                msgOpcode = *(uchar*)(plaintext+4); //plaintext has at least 5 bytes of memory allocated
            
                switch (msgOpcode){
                case ONLINE_CMD:
                    if(-1 == handle_get_online_users(comm_socket_id, plaintext)) {
                        log("Error on handle_get_online_users");
                        safe_free(session_key, session_key_len);
                        set_user_socket(get_username_by_user_id(client_user_id), -1);
                        close(comm_socket_id);
                        return 0;
                    }
                    break;

                case CHAT_CMD:
                    if(-1 == handle_chat_request(comm_socket_id, client_user_id, relay_msg, plaintext, plain_len)) {
                        log("Error on handle_chat_request");
                        safe_free(session_key, session_key_len);
                        set_user_socket(get_username_by_user_id(client_user_id), -1);
                        close(comm_socket_id);
                        return 0;
                    }
                    break;
                
                case CHAT_POS: 
                case CHAT_NEG:
                case STOP_CHAT:
                    if(-1 == handle_chat_pos_neg(plaintext, msgOpcode, plain_len)){
                        log("Error on handle_chat_pos_neg");
                        safe_free(session_key, session_key_len);
                        set_user_socket(get_username_by_user_id(client_user_id), -1);
                        close(comm_socket_id);
                        return 0;
                    }
                    break;
                case CHAT_RESPONSE:
                case AUTH:
                    ret = handle_auth_and_msg(plaintext, msgOpcode, plain_len);
                    if(ret<0) {
                        log("Error on handle_msg");
                        safe_free(session_key, session_key_len);
                        set_user_socket(get_username_by_user_id(client_user_id), -1);
                        close(comm_socket_id);
                        return 0;
                    }
                    break;
                case EXIT_CMD:
                    safe_free(session_key, session_key_len);
                    set_user_socket(get_username_by_user_id(client_user_id), -1);
                    close(comm_socket_id);
                    exit(0);
                default:
                    log("\n\n***** INVALID COMMAND *****\n\n");
                    break;
                }

            }
        }
        else if (pid == -1){
            log("ERROR on fork");
            return 0;
        }
        close(comm_socket_id);
    }
}