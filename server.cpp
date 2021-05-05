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

#include "constant.h"
#include "util.h"

using namespace std;


//Parameters of connection
const char* srv_ipv4 = "127.0.0.1";;
const int srv_port = 4242;

//TODO: maybe create a conf file to get configuration parameters eventually

int main(){
    int ret, listen_socket_id, comm_socket_id;
    struct sockaddr_in srv_addr, cl_addr;
    char send_buffer[1024];
    pid_t pid;

    
    //Preparation of ip address struct
    memset(&srv_addr, 0, sizeof(srv_addr));
    listen_socket_id = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_socket_id == -1)
        errorHandler(SOCK_ERR);    
    

    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(srv_port); 
    inet_pton(AF_INET, srv_ipv4, &srv_addr.sin_addr);
    cout << "[LOG] address struct preparation..." << endl;
    
    if(-1 == bind(listen_socket_id, (struct sockaddr*)&srv_addr, sizeof(srv_addr)))
        errorHandler(BIND_ERR);

    if(-1 == listen(listen_socket_id, SOCKET_QUEUE))
        errorHandler(LISTEN_ERR);

    unsigned int len = sizeof(cl_addr);
    log("Socket is listening...");

    while(true){
        comm_socket_id = accept(listen_socket_id, (struct sockaddr*)&cl_addr, &len);
        pid = fork();

        if(pid == 0){
            //Child process
            while(true){
                uint16_t sizeMsgClient;
                int size;
                char* msgClient;

                close(listen_socket_id);
                cout << "[LOG] connection established with client" << endl;

                //Send ACK to client
                strcpy(send_buffer, "ACK");
                len = strlen(send_buffer);
                
                //Obtain message length from client
                ret = recv(comm_socket_id, (void*)&sizeMsgClient, sizeof(uint16_t), 0);
                if(ret < 0)
                    errorHandler(REC_ERR);
                if(ret = 0)
                    vlog("No message from the server");
                
                size = ntohs(sizeMsgClient);
                if(size>INT_MAX/sizeof(char))
                    errorHandler(INT_OW_ERR);
                
                msgClient = (char*)malloc(sizeof(char)*size);
                if(!msgClient)
                    errorHandler(MALLOC_ERR);

                ret = recv(comm_socket_id, (void*)msgClient, size, 0);
                if(ret < 0)
                    errorHandler(REC_ERR);
                if(ret = 0)
                    vlog("No message from the server");


                string tmp(msgClient);
                log("Msg received is " + tmp);
                
                /*
                * TODO: DEMULTIPLEXING TO ADD
                */ 

                
                //For now just an echo
                ret = send(comm_socket_id, (void*)&sizeMsgClient, sizeof(uint16_t), 0);
                if(ret < 0 || ret!=sizeof(uint16_t))
                    errorHandler(SEND_ERR);

                ret = send(comm_socket_id, (void*)msgClient, size, 0);  
                if(ret < 0 || ret != size)
                    errorHandler(SEND_ERR);

                free(msgClient);
            }        
        } else if(pid == -1){
            errorHandler(FORK_ERR);
        }

        close(comm_socket_id);
    }
}