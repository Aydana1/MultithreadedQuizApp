#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <pthread.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/time.h>

#define    QLEN            5
#define maxNumOfGroups 32
#define    BUFSIZE         4096
int passivesock( char *service, char *protocol, int qlen, int *rport );

void fail(const char* message) {
    fputs(message, stderr);
    exit(-1);
}

pthread_mutex_t members_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t groups_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t fdset_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t send_quiz_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t firstAnswered_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scoring_lock = PTHREAD_MUTEX_INITIALIZER;

int groupCount = 0;
int members = 0;
char groupname[20] = {0};
char topic[20] = {0};
char username[20] = {0};
int groupsize = 0;

typedef struct {
    int fd;
    char username[20];
} Socket;

typedef struct {
    int gid;
    int members;
    char groupname[20];
    int groupsize;
    char topic[20];
    Socket clients[1010];
} Group;

Group groups[maxNumOfGroups];

fd_set       afds;
fd_set bfds;

char buf[BUFSIZE];
long cc;
int quiz_size = 0;
int size = 0;
FILE *tempFile;

typedef struct {
    char body[2048];
    char answer[2048];
} Question;

Question questions[128];

int score = 0;
_Bool isFirst = 0;
char firstToAns[30];
int questScores[128][1010];

typedef struct {
    int id;
    char name[30];
} Winner;

void *readQuizFile(void *socket) {

    int ssock = (int) socket;
    size = quiz_size;
    tempFile = fopen("/Users/ajdana/Desktop/Project_os/temp.txt", "w");

    if (tempFile == NULL) {
        printf("No such file!! \n");
        exit(-1);
    }

    while(quiz_size >= 0) {

        int cc = read(ssock, buf, BUFSIZE);
        fprintf(tempFile, buf);

        if (cc <= 0) {
            printf("couldn't read\n");
            pthread_exit(NULL);
        }

        buf[cc] = '\0';
        printf("FROM ADMIN: %s", buf);
        fflush(stdout);

        quiz_size -= cc;
        if (quiz_size <= 0) {
            break;
        }
    }

    fputs("\n", tempFile);
    fclose(tempFile);

    pthread_mutex_lock(&fdset_mutex);
    FD_SET(ssock, &afds);
    pthread_mutex_unlock(&fdset_mutex);

    pthread_exit(NULL);
}


void leaveGroup(int fd) {
    printf("Client left the group\n");
    for (int i = 0; i < groupCount; i++) {
        if(strcmp(groups[i].groupname, groupname) == 0) {
            pthread_mutex_lock(&members_mutex);
            groups[i].members--;
            pthread_mutex_unlock(&members_mutex);
        }
    }
    pthread_mutex_lock(&fdset_mutex);
    FD_CLR(fd, &bfds);   //remove it from a free set
    FD_SET(fd, &afds);  //add it to a new set
    pthread_mutex_unlock(&fdset_mutex);
}

void cancelGroup(int fd) {
    char *cancel = "OK|Group is canceled.\r\n";
    write(fd, cancel, strlen(cancel));
    pthread_mutex_lock(&groups_mutex);
    for (int i = 0; i < groupCount; i++) {
        if (groups[i].gid == groupCount-1 && groups[i].clients[0].fd == fd) {
            printf( "Group is canceled\n" ); //??
            groupCount--;
            groups[i].groupsize = 0;
            strcpy(groups[i].groupname, "");
            strcpy(groups[i].topic, "");
            printf("groupCount: %d", groupCount);
            break;
        }
    }
    pthread_mutex_unlock(&groups_mutex);
}

void *startQuiz(void *fds) {

    Socket *arr = (Socket *) fds;

    tempFile = fopen("/Users/ajdana/Desktop/Project_os/temp.txt", "r");
    if (tempFile == NULL) {
        printf("No such file!! \n");
        exit(-1);
    }

    char temp_body[2048];
    int line = 0;

    pthread_mutex_lock(&send_quiz_mutex);
    while (fgets(temp_body, 2048, tempFile) != NULL) {
        strcat(questions[line].body, temp_body);  //concatenate until you hit a new line
        if (strcmp(temp_body, "\n") == 0) {
            fgets(temp_body, 2048, tempFile);
            strcpy(questions[line].answer, temp_body);  //1st question is over
            line++;
            printf("line = %d\n", line);
            fgets(temp_body, 2048, tempFile); // new line
        }
    }
    pthread_mutex_unlock(&send_quiz_mutex);

    printf("lines = %d\n", line);

//    char *bad = "BAD|Admin cannot leave the group!\r\n";
//
//    if ((cc = read(arr[0].fd, buf, BUFSIZE)) <= 0) {
//    } else {
//        buf[cc] = '\0';
//        if(strcmp(buf, "CANCEL") == 0) {
//            cancelGroup(arr[0].fd);
//        }
//
//        if(strcmp(buf, "LEAVE") == 0) {
//            write(arr[0].fd, bad, strlen(bad));
//        }
//    }

    fd_set cfds;
    int nfds;
    nfds = arr[members-1].fd + 1;

    for (int i = 0; i <= members; i++) {
        if ( arr[i].fd+1 > nfds ){
            nfds = arr[i].fd+1; //nfds is the highest socket number + 1
        }
    }

    int qid = 0;

    struct timeval timeout;
    timeout.tv_sec = 60;
    timeout.tv_usec = 0;

    int rc = 0;
    int timeCount = 0;
    int sumOfScores = 0;
    Winner firstAnswered[128];

    while(qid < line) {

            sprintf(buf, "QUES|%d|%s", strlen(questions[qid].body), questions[qid].body);
            printf("QUESTION %d: %s \n", qid, questions[qid].body);

            for (int i = 0; i <= members; i++) {
                write(arr[i].fd, buf, strlen(buf));
            }

            int answeredClientNum = 0;
            int i=1;

            while (answeredClientNum != members) {   // && timeCount < 20

                memcpy((char *) &cfds, (char *) &bfds, sizeof(cfds));   // make a copy of  &afds

                if ((rc = select(nfds, &cfds, (fd_set *) 0, (fd_set *) 0, &timeout)) < 0) {  //&timeout
                    fprintf(stderr, "server select: %s\n", strerror(errno));
                    fclose(tempFile);
                    pthread_exit(NULL);
                }
                else {
                    printf("number  of sockets ready to send: %d\n", rc);
                }

                if(rc == 0) {
                    printf("NO ONE IS READY TO READ!\n");
                    timeCount += 60;
                    printf("timeCount = %d\n", timeCount);
                }

                if (FD_ISSET(arr[i].fd, &cfds)) {
                  //  printf("FDSET IS READY TO READ!\n");
                    if ((cc = read(arr[i].fd, buf, BUFSIZE)) <= 0) {
                        (void) close(arr[i].fd);
                        pthread_mutex_lock(&fdset_mutex);
                        FD_CLR(arr[i].fd, &cfds);
                        FD_SET(arr[i].fd, &afds);
                        pthread_mutex_unlock(&fdset_mutex);
                        } else {
                            buf[cc] = '\0';

                            if(strcmp(buf, "LEAVE") == 0) {
                                leaveGroup(arr[i].fd);
                            }

                                printf("3.FROM CLIENT %d: %s", arr[i].fd, buf);
                                answeredClientNum++;

                                char *s;
                                char *token;
                                char *msg;
                                msg = strtok_r(buf, "|", &s);
                                char ans[10] = {0};
                                token = strtok_r(NULL, "|", &s);
                                strcpy(ans, token);

                                pthread_mutex_lock(&firstAnswered_lock);

                                if (strcmp(msg, "ANS") == 0) {
                                    printf("correct answer: %s",questions[qid].answer);
                                    if (strcmp(questions[qid].answer, ans) == 0) {
                                        score = 1;
                                    } else if (strcmp(questions[qid].answer, ans) != 0) {
                                        score = -1;
                                    }
                                    if (isFirst == 0) {
                                        isFirst = 1;
                                        strcpy(firstToAns, arr[i].username);    // WINNER NAME
                                    }
                                    if (strcmp(firstToAns, arr[i].username) == 0 && score == 1) {
                                        score = 2;
                                        strcpy(firstAnswered[qid].name, arr[i].username);
                                        firstAnswered[qid].id = arr[i].fd;
                                    }
                                } else if (strcmp(ans, "NOANS") == 0) {
                                    score = 0;
                                }

                                questScores[qid][arr[i].fd] = score;
                                printf("Score of fd #%d = %d\n", arr[i].fd, questScores[qid][arr[i].fd]);

                                pthread_mutex_unlock(&firstAnswered_lock);

                        }
                }

                if(timeCount == 60) {      // Kick out the player if it is timeout!
                    char *msg = "YOU'RE KICKED OUT!\r\n";
                    write(arr[i].fd, msg, strlen(msg));
                    printf("TIMEOUT!!!\n");
                    pthread_mutex_lock(&members_mutex);
                    members--;
                    pthread_mutex_unlock(&members_mutex);
                    pthread_mutex_lock(&fdset_mutex);
                    FD_CLR(arr[i].fd, &cfds);
                    FD_SET(arr[i].fd, &afds);
                    pthread_mutex_unlock(&fdset_mutex);
                }

                // printf("YOU HIT ME! Clients answered = %d \n", answeredClientNum);

                if(i == members + 1) {
                  //  printf("Value of clients answered = %d\n", answeredClientNum);
                    i=0;
                }
                i++;

            }

            for (int k = 0; k <= members; k++) {
                if (firstAnswered[qid].id == arr[k].fd) {        //if current client is a winner announce to everyone
                    for (int m = 0; m <= members; m++) {
                        sprintf(buf, "WIN|%s\n", firstAnswered[qid].name);
                        write(arr[m].fd, buf, strlen(buf));
                    }
                }
            }

            printf("winner is %s\n", firstAnswered[qid].name);

        qid++;
    }

    char *res = "RESULTS|";

    if(qid == line) {
            printf("QUIZ IS OVER!\n");
            for (int i = 0; i <= members; i++) {
                pthread_mutex_lock(&scoring_lock);
                for(int k=0; k<line; k++) {
                    sumOfScores += questScores[k][arr[i+1].fd];
                }
                write(arr[i].fd, res, strlen(res));
                sprintf(buf, "%s|%d|", arr[i+1].username,sumOfScores);
                write(arr[i].fd, buf, strlen(buf));
                printf("%s", buf);
                sumOfScores = 0;
                pthread_mutex_unlock(&scoring_lock);

                pthread_mutex_lock(&groups_mutex);
                for(int j=0; j<groupCount; j++) {
                    if(groups[j].clients[0].fd == arr[0].fd) {
                        groupCount--;
                        groups[j].groupsize = 0;
                        strcpy(groups[j].groupname, "");
                        strcpy(groups[j].topic, "");
                    }
                }
                pthread_mutex_unlock(&groups_mutex);

                pthread_mutex_lock(&fdset_mutex);
                if( FD_ISSET(arr[i].fd, &cfds)){
                    FD_CLR(arr[i].fd, &cfds);
                    FD_SET(arr[i].fd, &afds);
                }
                pthread_mutex_unlock(&fdset_mutex);
            }

        pthread_exit(NULL);
    }
}

pthread_t threads;
int status;

void formNewGroup(int fd, char topic[20], char groupname[20], int groupsize) {

        char *p = "BAD|The groupname is not unique.\r\n";
        _Bool exists = 0;

        // if such a group already exists, send BAD msg
        for (int i = 0; i < groupCount; i++) {
            if (strcmp(groups[i].groupname, groupname) == 0) {  //if it is existing group
                write(fd, p, strlen(p));
                exists = 1;
            }
        }

        //if groupname is not in use, create one
        if(!exists) {
            //protect with mutex, since groupCount is changed by each new thread per group
            pthread_mutex_lock(&groups_mutex);
            write(fd, "SENDQUIZ\r\n", 10);
            groups[groupCount].clients[0].fd = fd;   // assign socket to admin
            groups[groupCount].gid = groupCount;     //add matching id to the group
            strcpy(groups[groupCount].groupname, groupname);
            groups[groupCount].groupsize = groupsize;
            strcpy(groups[groupCount].topic, topic);
            strcpy(groups[groupCount].clients[0].username, "Admin");
            groupCount++;
            printf("Number of groups: %d\n", groupCount);
            pthread_mutex_unlock(&groups_mutex);
        }
}

int j = 0;

void joinGroup(int fd, char groupname[20], char username[20]) {

    _Bool exists = 0;
    char *p = "FULL\r\n";
    char *v = "BAD|Admin cannot join a group!\r\n";

    // add a new client to the existing group
    for (int i = 0; i < groupCount; i++) {
        if (strcmp(groups[i].groupname, groupname) == 0) {  //if it is existing group
            exists = 1;
            //if group is already full, send BAD msg
            if (groups[i].groupsize <= members) {
                write(fd, p, strlen(p));
                break;
            }
            else {
                if (groups[i].gid == groupCount-1) {
                    //protect with mutex as members value is changed by multiple clients
                    pthread_mutex_lock(&members_mutex);
                    members++;
                    groups[i].clients[j+1].fd = fd;
                    strcpy(groups[i].clients[j+1].username, username); //add the username
                    j++;
                    groups[i].members = members;

                    //protect with mutex since multiple clients can join the group at the same time
                    pthread_mutex_lock(&fdset_mutex);
                    FD_CLR(fd, &afds);   //remove it from a free set
                    FD_SET(fd, &bfds);  //add it to a new set
                    FD_CLR(groups[i].clients[0].fd, &afds);   //remove admin from a free set
                    FD_SET(groups[i].clients[0].fd, &bfds);
                    pthread_mutex_unlock(&fdset_mutex);

                    pthread_mutex_unlock(&members_mutex);

                    write(fd, "OK\r\n", 4);  //send OK msg
                    printf("username is %s,group is %s \n", groups[i].clients[j].username, groups[i].groupname);
                    break;
                }
            }
        }
    }
    if(!exists) {  //if there is no such group exists
        write(fd, "NOGROUP\r\n", 9);
    }

    // ******************* when groups get full *********************** //
    pthread_mutex_lock(&send_quiz_mutex);
    for(int t=0; t< groupCount; t++) {
        while(groups[t].groupsize == groups[t].members) {

            for(int i=0; i<=members; ++i) {
                printf("I AM SOCKET %d\n", groups[t].clients[i].fd);
            }

            status = pthread_create( &threads, NULL, startQuiz, (void*) groups[t].clients);   // pass array of fds in this group
            if ( status != 0 )
            {
                printf( "pthread_create error %d.\n", status);
                exit( -1 );
            }
            break;
        }
    }
    pthread_mutex_unlock(&send_quiz_mutex);
}

void sendQuizToServer(int fd) {

    pthread_mutex_lock(&fdset_mutex);
    FD_CLR(fd, &afds);  //remove this fd from a free set
    pthread_mutex_unlock(&fdset_mutex);

    status = pthread_create( &threads, NULL, readQuizFile, (void*) fd);   //send group leader's fd to the thread
    if ( status != 0 )
    {
        printf( "pthread_create error %d.\n", status);
        exit( -1 );
    }
}

void getGroupList(int fd) {

    char newbuf[BUFSIZE];
    char *g = "OPENGROUPS|\r\n";

    if(groupCount != 0) {
        for(int t=0; t< groupCount; t++) {
            if(groups[t].groupsize != groups[t].members) {   //if the group is not full
                sprintf(newbuf, "OPENGROUPS|%s|%s|%d|%d\r\n", groups[t].topic, groups[t].groupname, groups[t].groupsize, groups[t].members);
                write(fd, newbuf, strlen(newbuf));
                printf("OPENGROUPS|%s|%s|%d|%d\r\n", groups[t].topic, groups[t].groupname, groups[t].groupsize, groups[t].members);
            } else {
                write(fd, g, strlen(g));
            }
        }
    } else {
        write(fd, g, strlen(g));
    }
}

int main( int argc, char *argv[] ) {

    char         buf[BUFSIZE];
    char         *service;
    struct sockaddr_in fsin;
    int          msock;
    int          ssock;
    int          alen;
    fd_set       rfds;
    int          fd;
    int          nfds;
    int          rport = 0;
    int          cc;

    switch (argc)
    {
        case   1:
            // No args? let the OS choose a port and tell the user
            rport = 1;
            break;
        case   2:
            // User provides a port? then use it
            service = argv[1];
            break;
        default:
            fprintf( stderr, "usage: server [port]\n" );
            exit(-1);
    }

    msock = passivesock( service, "tcp", QLEN, &rport );
    if (rport)
    {
        // Tell the user the selected port
        printf( "server: port %d\n", rport );
        fflush( stdout );
    }

    nfds = msock+1;

    FD_ZERO(&bfds);
    FD_ZERO(&afds); // set elements of afds array to zero
    FD_SET( msock, &afds );   // set mainsock to 1
    for (;;)
    {
        memcpy((char *)&rfds, (char *)&afds, sizeof(rfds));   // make a copy of  &afds

        if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0,
                   (struct timeval *)0) < 0)
        {
            fprintf( stderr, "server select: %s\n", strerror(errno) );
            exit(-1);
        }

        if (FD_ISSET( msock, &rfds))
        {
            int    ssock;

            // we can call accept with no fear of blocking
            alen = sizeof(fsin);
            ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
            if (ssock < 0)
            {
                fprintf( stderr, "accept: %s\n", strerror(errno) );
                exit(-1);
            }

            /* start listening to this guy */
            pthread_mutex_lock(&fdset_mutex);
            FD_SET( ssock, &afds );
            if(groupCount != 0) {
                for(int t=0; t< groupCount; t++) {
                    if(groups[t].groupsize != groups[t].members) {
                        sprintf(buf, "OPENGROUPS|%s|%s|%d|%d\r\n", groups[t].topic, groups[t].groupname, groups[t].groupsize, groups[t].members);
                        write(ssock, buf, strlen(buf));
                        printf("OPENGROUPS|%s|%s|%d|%d\r\n", groups[t].topic, groups[t].groupname, groups[t].groupsize, groups[t].members);
                    }
                }
            }
            pthread_mutex_unlock(&fdset_mutex);

            // increase the maximum
            if ( ssock+1 > nfds ){
                nfds = ssock+1;
            }
        }

        /* Handle the participants requests  */
        for ( fd = 0; fd < nfds; fd++ )
        {
            // check every socket to see if it's in the ready set
            if (fd != msock && FD_ISSET(fd, &rfds))
            {

                // read without blocking because data is there
                if ( (cc = read( fd, buf, BUFSIZE )) <= 0 )
                {

                    printf( "The client has gone.\n" );
                    (void) close(fd);
                    pthread_mutex_lock(&fdset_mutex);
                    FD_CLR( fd, &afds );
                    pthread_mutex_unlock(&fdset_mutex);

                    // lower the max socket number if needed
                    if ( nfds == fd+1 )
                        nfds--;
                }
                else
                {
                    buf[cc] = '\0';
                    printf( "2.The client says: %s\n", buf );
                    char temp[BUFSIZE];
                    strcpy(temp, buf);

                    char *bad = "BAD|Admin cannot leave the group!\r\n";

                    if(strcmp(buf, "CANCEL") == 0) {
                        cancelGroup(fd);
                    }

                    if(strcmp(buf, "LEAVE") == 0) {
                        for(int i=0; i<groupCount; i++) {
                            if (groups[i].clients[0].fd == fd) {
                                write(fd, bad, strlen(bad));
                            } else {
                                leaveGroup(fd);
                            }
                        }
                    }

                    char *s;
                    char *token;
                    char *msg;

                    msg = strtok_r(buf, "|", &s);

                    if (strcmp(msg, "GROUP") == 0) {
                        token = strtok_r(NULL, "|", &s);
                        strcpy(topic, token);
                        token = strtok_r(NULL, "|", &s);
                        strcpy(groupname, token);
                        token = strtok_r(NULL, "|", &s);
                        groupsize = atoi(token);
                        formNewGroup(fd, topic, groupname, groupsize);
                    }

                    if (strcmp(msg, "QUIZ") == 0) {
                        sscanf(temp, "QUIZ|%d|", &quiz_size);
                        sendQuizToServer(fd);
                    }

                    if(strcmp(temp, "GETOPENGROUPS\n") == 0) {
                         getGroupList(fd);
                    }

                    if (strcmp(msg, "JOIN") == 0) {
                        token = strtok_r(NULL, "|", &s);
                        strcpy(groupname, token);
                        token = strtok_r(NULL, "|", &s);
                        strcpy(username, token);
                        //printf("MY username: %s\n", username);
                        joinGroup(fd, groupname, username);
                    }
                }
            }
        }
    }
}





