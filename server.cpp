/*
    Matthew Schutz
    CS4850
    ProjectV2
    03/13/2023
    This file will run a server for a chat room application. The server will allow MAX_CLIENTS to connect at a time using multithreading. This
    project uses header files that I believe are only compilable on MacOS.
 */

#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <fstream>
#include <thread>
#include <mutex>

// used to track open threads
struct ThInfo {
    int thID;
    std::thread th;
    std::string userName;
} typedef threadInfo;

// used to record logins and login status
struct UserInfo{
    std::string password;
    int loggedIn;
} typedef userInfo;

#define PORT 16271
#define MAX_LINE 294 // For send userID must be able to support the length of the command(4) + length of userID(up to 32) + length of message(up to 256) + spaces between(2)
#define MAX_CLIENTS 3

void handleClient(int clientSock);
int login(char* buf,int s);
int newuser(char* buf, int s);
int sendMessage(char* buf, int s);
int logout(char* buf, int s);
int who(char* buf, int s);
std::vector<int>* getRecipients(std::string recipient, std::string sender);
void syncPrint(std::string str);

std::unordered_map<std::string, userInfo> usersMap;
std::unordered_map<int, threadInfo*> threads;
std::string loggedInUser;
int threadNum = 0;
std::mutex clientMtx, coutMtx;

int main(int argc, const char * argv[]) {

    // open the file that contains usernames and passwords
    std::ifstream usersFile;
    usersFile.open("users.txt");
    std::string line;

    // if the file exists, add each username-password combination to a hash map which will be used to check clients credentials
    if(usersFile.is_open()){
        while (getline(usersFile, line)) {
            std::stringstream ss(line);
            std::string substr;
            getline(ss, substr, ',');
            std::string userID = substr.substr(1);
            ss >> substr;
            std::string password = substr.substr(0, substr.size()-1);
            usersMap[userID].password = password;
            usersMap[userID].loggedIn = 1;
        }
    }
    // close the file
    usersFile.close();

    // open a socket using IPv4 and TCP
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);

    // error checking to ensure the socket is created
    if(listenSocket == -1){
        std::cerr << "Error creating socket" << std::endl;
        close(listenSocket);
        return -1;
    }

    // binding our socket to the address(ip+port) with error checking
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if(bind(listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == -1){
        std::cerr << "Error binding socket" << std::endl;
        close(listenSocket);
        return -1;
    }

    // listen on a socket for client connections. Check to ensure this is accomplished
    if(listen(listenSocket, SOMAXCONN) == -1){
        std::cerr << "Error listening on socket" << std::endl;
        close(listenSocket);
        return -1;
    }

    // welcome
    std::cout << "My chat room server. Version Two.  \n" << std::endl;

    // placeholder for new connections
    int s;
    while(1){
        // accept a new connection and check for errors
        s = accept(listenSocket, NULL, NULL);
        if(s==-1){
            std::cerr << "accept() error" << std::endl;
            break;
        }
        // check to make sure that max number of clients hasn't already been reached. If it has, print a message, send a message to the client informing them of this,
        // and then close the socket.
        if(threads.size() >= MAX_CLIENTS){
            std::cout << "Maximum number of clients already connected to server." << std::endl;
            char buf[] = "Maximum number of clients already connected to server. Please try again later.\0";
            send(s, buf, strlen(buf), 0);
            close(s);
            continue;
        }
        // create a new thread for this socket
        std::thread t(handleClient, s);
        std::lock_guard<std::mutex> guard(clientMtx);
        threadInfo *tI = new threadInfo();
        tI->th = move(t);
        tI->thID = threadNum;
        tI->userName = "";
        threads[s] = tI;
        threadNum++;
    }
    close(listenSocket);
    return 0;
}

// function that logs the current user out. Returns 0 on success, -1 on failure
int logout(char* buf, int s){
    // makes sure a user is first logged in
    if (threads[s]->userName == "") {
        strcpy(buf, "Logout failed.");
        send(s, buf, strlen(buf), 0);
        return -1;
    }

    // returns a message to the client notifying the user logged out.
    std::string out = threads[s]->userName;
    out.append(" logout.");
    syncPrint(out);

    // create a message to send to all logged in users, notifying them that this user has logged out
    strcpy(buf, threads[s]->userName.c_str());
    strcpy(buf + threads[s]->userName.length(), " left.\0");
    
    // send the message to all logged in users
    std::unordered_map<int, threadInfo*>::iterator it;
    for(it = threads.begin(); it != threads.end(); ++it){
        if (it->second->userName.length() != 0) {
            send(it->first, buf, strlen(buf), 0);
        }
    }
    // update the logged in status of this user
    usersMap[threads[s]->userName].loggedIn = 1;
    return 0;
}

// allows a client to send a message. Will receive the message and then append it to the username and send it back to client
// returns 0 on success and -1 on failure.
int sendMessage(char* buf, int s){
    // separate the command and designated recipient
    std::stringstream inputStream(buf);
    std::string command, recipient, msg;
    inputStream >> command;
    inputStream >> recipient;
    
    // isolate the message. Iterate through the stringstream until it has found the the space before the content of the message.
    // i starts at 7 as it can be guaranteed that each message consists of at least "send " + a three letter minimum recipient
    msg = inputStream.str();
    int i = 7;
    while(msg.at(i) != ' ') i++;
    msg = msg.substr(i+1, msg.length()-i-2);
    
    // vector for all recipients of this message. If recipient list is empty, return and do nothing
    std::vector<int>* recipientList = getRecipients(recipient, threads[s]->userName);
    if(recipientList->size() == 0){
        delete recipientList;
        return -1;
    }
 
    // create a correctly formatted message
    char messageWithUser[MAX_LINE];
    memset(messageWithUser, 0, MAX_LINE);
    strcpy(messageWithUser, threads[s]->userName.c_str());
    messageWithUser[threads[s]->userName.length()] = ':';
    messageWithUser[threads[s]->userName.length()+1] = ' ';
    strcpy(messageWithUser + threads[s]->userName.length() + 2, msg.c_str());
    messageWithUser[threads[s]->userName.length() + msg.length() + 2] = '\0';
    if(recipient == "all"){
        std::string out;
        syncPrint(out.append(messageWithUser));
    }
    else{
        std::string serverOutput = threads[s]->userName;
        serverOutput.append(" (to ");
        serverOutput.append(recipient);
        serverOutput.append("): ");
        serverOutput.append(msg);
        syncPrint(serverOutput);
    }
    
    // send the message to all recipients
    for(int s : *recipientList){
        send(s, messageWithUser, strlen(messageWithUser), 0);
    }

    delete recipientList;
    return 0;
}

// will get a list of sockets that are recipients based on the designated recipient and sender. Will not send the message to the sender.
// returns a vector of recipients sockets, or an empty vector if no recipients
std::vector<int>* getRecipients(std::string recipient, std::string sender){
    std::vector<int>* recipientList = new std::vector<int>();
    // add all open sockets that have a logged in user besides the sender
    if(recipient == "all"){
        std::unordered_map<int, threadInfo*>::iterator it;
        for (it = threads.begin(); it != threads.end(); ++it) {
            if(it->second->userName != sender && it->second->userName.length() != 0){
                recipientList->push_back(it->first);
            }
        }
    }
    // adds the specified recipient if they exist, are logged in, and are not the sender
    else{
        if(sender == recipient) return recipientList;
        std::unordered_map<int, threadInfo*>::iterator it;
        for (it = threads.begin(); it != threads.end(); ++it) {
            if(it->second->userName == recipient){
                recipientList->push_back(it->first);
                return recipientList;
            }
        }
        return recipientList;
    }
    
    return recipientList;
}

// allows client to create a new user, and adds it to the existing base of users. Sends a confirmation back to user.
// returns 0 on success and -1 on failure.
int newuser(char* buf, int s){
    std::stringstream inputStream(buf);
    std::string command, userID, password;
    inputStream >> command;
    inputStream >> userID;
    inputStream >> password;

    // check to see if user already exists
    if (usersMap.find(userID) != usersMap.end()) {
        strcpy(buf, "Denied. User account already exists.");
    } else { // create user account. if users.txt file does not exist, then create file
        strcpy(buf, "New user account created. Please login.");
        usersMap[userID].password = password;
        usersMap[userID].loggedIn = 1;
        std::cout << "New user account created." << std::endl;
        std::ofstream usersFile;
        usersFile.open("users.txt", std::ios::app);
        usersFile << "(" << userID << ", " << password << ")\n";
        usersFile.close();
    }
    // send back the message, whether its an error message or confirmation message
    send(s, buf, strlen(buf), 0);
    return 0;
}

// logs a client in. returns a message either confirming the login or notifying the user of the failed login
int login(char* buf,int s) {
    // seperate the command, userID, and password
    std::stringstream inputStream(buf);
    std::string command, userID, password;
    inputStream >> command;
    inputStream >> userID;
    inputStream >> password;
    // check to make sure the userID exists, the password matches, and the user isn't already logged in
    if (usersMap.find(userID) == usersMap.end()) {
        strcpy(buf, "Denied. User name or password incorrect.");
    }else if(usersMap[userID].password != password){
        strcpy(buf, "Denied. User name or password incorrect.");
    } else if(usersMap[userID].loggedIn == 0){
        strcpy(buf, "Denied. User is already logged in.");
    } else{
        // log the user in and then send a confirmation message back and log the confirmation on the server
        strcpy(buf, "login confirmed.");
        std::string out = userID;
        out.append(" login.");
        syncPrint(out);
        // update threads and usersMap with the new login status
        threads[s]->userName = userID;
        usersMap[userID].loggedIn = 0;
        }
    send(s, buf, strlen(buf), 0);
    memset(buf, 0, MAX_LINE);
    strcpy(buf, userID.c_str());
    strcpy(buf+userID.length(), " joins.");
    
    // send a notice of the new login to all logged in users, besides the newly logged in user
    std::unordered_map<int, threadInfo*>::iterator it;
    for(it = threads.begin(); it != threads.end(); ++it){
        if (it->second->userName.length() != 0 && it->second->userName != userID) {
            send(it->first, buf, strlen(buf), 0);
        }
    }
    
    return 0;
}

// sends a message back to the client of all logged in users
int who(char* buf, int s){
    std::string msg = "";
    // add all users to the message if they are logged in
    std::unordered_map<int, threadInfo*>::iterator it;
    for(it = threads.begin(); it != threads.end(); it++){
        if(it->second->userName.length() != 0){
            if(msg.length() != 0){
                msg.append(", ");
            }
            msg.append(it->second->userName);
        }
    }
    // send the message
    strcpy(buf, msg.c_str());
    send(s, buf, strlen(buf), 0);
    return 0;
}

// function to handle each client thread. It will continually recieve messages from each client and process the commands
void handleClient(int clientSock){
    // create a buffer. Size must be MAX_LINE + 5, because the send command can send a message of 256 characters, and must also account for the command included in
    // that message.
    char buf[MAX_LINE];
    while(1){
        // receive a message from a client
        int len = recv(clientSock, buf, MAX_LINE, 0);
        // check to see if client is connected. If client is not connected, the server will break out of the connection and await a new connection.
        if (len == 0) {
            break;
        }
        // add terminating null char
        buf[len] = 0;
        // string stream to hold the command from the input
        std::stringstream inputStream(buf);
        std::string command;
        inputStream >> command;
        
        // if else statements for each command
        if (command == "login") {
            login(buf,clientSock);
        } else if(command == "newuser"){
            newuser(buf, clientSock);
        } else if(command == "who"){
            who(buf, clientSock);
        } else if(command == "logout"){
            if(logout(buf, clientSock) == 0){
                break;
            }
        } else if (command == "send"){
            sendMessage(buf, clientSock);
        }
    }
    // lock while cleaning up this thread to avoid any messes
    std::lock_guard<std::mutex> guard(clientMtx);
    threads[clientSock]->th.detach();
    // erase the thread from the map of threads
    delete threads[clientSock];
    threads.erase(clientSock);
    close(clientSock);
    return;
}

// used to safely print messages to ensure multiple threads don't print at the same time
void syncPrint(std::string str){
    std::lock_guard<std::mutex> guard(coutMtx);
    std::cout << str;
    std::cout << std::endl;
    return;
}
