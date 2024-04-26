#include <stdio.h>
#include <signal.h>
#include "csapp.h"
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void *thread(void *vargp);
void doit(int clientfd);
void read_requesthdrs(rio_t *rp, void *buf, int serverfd, char *hostname, char *port);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *hostname, char *port, char *filename);

/* Cache 처리 위한 변수, 구조체, 함수 */
int current_cache_size = 0; // total cache size 할당 변수
typedef struct web_object_t // Cache List 생성 위한 구조체
{
  char filename[MAXLINE];
  int content_length;
  char *response_ptr;
  struct web_object_t *prev, *next;
} web_object_t; 

web_object_t *start_ptr;    // Cache Linked List 시작 지점의 pointer
web_object_t *end_ptr;      // Cache Linked List 마지막 지점의 pointer

web_object_t *find_cache(char *filename);
void send_cache(web_object_t *web_object, int clientfd);
void update_cache(web_object_t *web_object);
void write_cache(web_object_t *web_object);


static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv)
{
  int listenfd, *clientfd;
  char client_hostname[MAXLINE], client_port[MAXLINE];
  socklen_t clientlen;
  pthread_t tid;
  struct sockaddr_storage clientaddr;

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]); // 전달받은 포트 번호를 사용해 proxy의 수신 소켓 생성
  while (1)
  {
    clientlen = sizeof(clientaddr);
    clientfd = Malloc(sizeof(int));
    *clientfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // 클라이언트 연결 요청 수신
    Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);
    Pthread_create(&tid, NULL, thread, clientfd);                // concurrent 접속
  }
}

void *thread(void *vargp)
{
  start_ptr = (web_object_t *)Malloc(sizeof(web_object_t)); 
  end_ptr = (web_object_t *)Malloc(sizeof(web_object_t));
  int clientfd = *((int *)vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  doit(clientfd);
  Close(clientfd);
  return NULL;
}

void doit(int clientfd)
{
  int serverfd, content_length;
  char request_buf[MAXLINE], response_buf[MAXLINE];
  char method[MAXLINE], uri[MAXLINE], filename[MAXLINE], hostname[MAXLINE], port[MAXLINE];
  char *response_ptr, cgiargs[MAXLINE];
  rio_t request_rio, response_rio;

  /* Client의 Request Line 읽기(client->proxy) */
  Rio_readinitb(&request_rio, clientfd);
  Rio_readlineb(&request_rio, request_buf, MAXLINE);
  printf("Request headers:\n %s\n", request_buf);
  
  sscanf(request_buf, "%s %s", method, uri); // client request의 method, uri
  parse_uri(uri, hostname, port, filename);

  // Proxy -> Server 전송 시 항상 HTTP/1.0으로 request
  sprintf(request_buf, "%s %s %s\r\n", method, filename, "HTTP/1.0");

  // Cache되어 있는 request인지 확인
  web_object_t *finded_object = find_cache(filename);
  
  /* Cache되어 있는 경우, Proxy -> client response*/
  if (finded_object)
  {
    send_cache(finded_object, clientfd); // proxy->client, 바로 전송
    update_cache(finded_object);         // 사용한 웹 객체의 순서를 맨 앞으로 갱신
    return;                              // Server로 요청을 보내지 않고 통신 종료
  }

  /* Cache되어 있지 않은 경우, Proxy -> Server request */
  serverfd = Open_clientfd(hostname, port); // Server 소켓 생성
  if (serverfd < 0)                                           // hostname, port에 해당하는 address를 읽어올 수 없을 때, serverfd에는 -2가 할당됩니다(error).
  {
    clienterror(serverfd, method, "502", "Bad Gateway", "Failed to establish connection with the end server");
    return;
  }
  Rio_writen(serverfd, request_buf, strlen(request_buf));
  
  /* client의 request Header를 Server로 전송(proxy -> Server) */
  read_requesthdrs(&request_rio, request_buf, serverfd, hostname, port);

  Rio_readinitb(&response_rio, serverfd);                     // Server의 Response를 읽고, 
  while (strcmp(response_buf, "\r\n"))
  {
    Rio_readlineb(&response_rio, response_buf, MAXLINE);
    if (strstr(response_buf, "Content-length"))               // Content-length Header에 도달했을 때(Ex. content-length: 1234)
      content_length = atoi(strchr(response_buf, ':') + 1);   // 형 변환 후 할당 ('1234' -> 1234)
    Rio_writen(clientfd, response_buf, strlen(response_buf)); 
  }
  
  /* Server의 response body를 Client로 전송(Server -> Client) */
  response_ptr = Malloc(content_length);
  Rio_readnb(&response_rio, response_ptr, content_length);    // response의 body 읽기
  Rio_writen(clientfd, response_ptr, content_length);         // Client에 Response Body 전송
  
  /* Sever로부터 처리한 response 크기가 Cache 가능할 때 Cache 처리 */
  if (content_length <= MAX_OBJECT_SIZE)
  {
    web_object_t *web_object = (web_object_t *)malloc(sizeof(web_object_t));
    web_object->response_ptr = response_ptr;
    web_object->content_length = content_length;
    strcpy(web_object->filename, filename);
    write_cache(web_object); // web_object 완성 후 연결 리스트(캐시)에 연결시킴
  }

  Close(serverfd);
}

void parse_uri(char *uri, char *hostname, char *port, char *filename)
{
  // (scheme: //host:port/path?query#fragment)에 따라 '//'가 uri에 있으면, //를 제외 후 hostname pointer 위치됨(반대로 '//'가 있으면, 바로 uri시작 위치 = hostname pointer)
  char *uri_position_check = strstr(uri, "//");
  char *hostname_ptr = uri_position_check == NULL ? uri : uri_position_check + 2;
  char *port_ptr = strchr(hostname_ptr, ':');     // uri 내에서 port 위치 pointer 계산
  char *filename_ptr = strchr(hostname_ptr, '/'); // uri 내에서 filename 위치 pointer 계산
  
  strcpy(filename, filename_ptr);
  strncpy(port, port_ptr + 1, filename_ptr - port_ptr - 1); 
  strncpy(hostname, hostname_ptr, port_ptr - hostname_ptr);
}

/* request header 읽기 및 전송(Proxy -> Server)*/
void read_requesthdrs(rio_t *request_rio, void *request_buf, int serverfd, char *hostname, char *port)
{
  Rio_readlineb(request_rio, request_buf, MAXLINE); // 첫번째 줄 읽기

  while (strcmp(request_buf, "\r\n"))
  {
    Rio_writen(serverfd, request_buf, strlen(request_buf)); // Server에 전송
    Rio_readlineb(request_rio, request_buf, MAXLINE);       // 다음 줄 읽기
  }

  sprintf(request_buf, "\r\n");
  Rio_writen(serverfd, request_buf, strlen(request_buf));
  return;
}

// 캐싱된 웹 객체 중에 해당 `filename`을 가진 객체를 반환하는 함수
web_object_t *find_cache(char *filename)
{
  if (!start_ptr) // 캐시가 비었으면
    return NULL;
  web_object_t *current = start_ptr;          // 검사를 시작할 노드
  while (strcmp(current->filename, filename)) // 현재 검사 중인 노드의 filename이 찾는 filename과 다르면 반복
  {
    if (!current->next)                       // 현재 검사 중인 노드의 다음 노드가 없으면 NULL 반환
      return NULL;

    current = current->next;                  // 다음 노드로 이동
    if (!strcmp(current->filename, filename)) // filename이 같은 노드를 찾았다면 해당 객체 반환
      return current;
  }
  return current;
}

// `web_object`에 저장된 response를 Client에 전송하는 함수
void send_cache(web_object_t *web_object, int clientfd)
{
  /* response Header 생성 및 전송(proxy -> client) */
  char buf[MAXLINE];
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n\r\n", buf, web_object->content_length);
  Rio_writen(clientfd, buf, strlen(buf));

  Rio_writen(clientfd, web_object->response_ptr, web_object->content_length); // (캐쉬되었던) 웹 객체의 Response Body를 client에 전송
}

/* 사용한 `web_object`를 캐시 연결리스트의 start로 갱신하는 함수(LRU 변형) */
void update_cache(web_object_t *web_object)
{
  /* 예외 처리 */
  if (web_object == start_ptr)
    return;

  /* 연결리스트 업데이트 */
  if (web_object->next) // 연결 리스트의 맨 마지막에 있는 web object가 '아닐' 때
  {
    web_object->prev->next = web_object->next; // 업데이트하고자 하는 web object의 '앞 요소'와 업데이트하고자 하는 web object의 '뒤 요소'를 연결
    web_object->next->prev = web_object->prev;
  }
  else                  // 연결 리스트의 맨 마지막에 있는 web object'일' 때
  {
    web_object->prev->next = NULL;             // 업데이트하고자 하는 web object의 '앞 요소'와 업데이트하고자 하는 web object의 '뒤 요소' NULL(없음)을 연결
  }

  /* 업데이트하고자 하는 web object를 start pointer로 갱신 */
  web_object->next = start_ptr; // 업데이트하고자 하는 web object의 '뒤 요소'를 현재 연결리스트 시작 포인터로 연결
  start_ptr = web_object;       // 현재 연결리스트의 시작 포인터 갱신
}

/* web object를 캐시 연결 리스트에 연결함 */
void write_cache(web_object_t *web_object)
{
  current_cache_size += web_object->content_length; // 현재 Cache 사이즈를 갱신

  /* MAX_CACHE_SIZE 초과 시 가장 뒤에 있는 web object 먼저 제거(LRU 변형) */
  while (current_cache_size > MAX_CACHE_SIZE) 
  {
    current_cache_size -= end_ptr->content_length;
    end_ptr = end_ptr->prev;
    free(end_ptr->next);
    end_ptr->next = NULL;
  }

  /* 연결하고자 하는 web objcet와 start pointer 연결 */
  web_object->next = start_ptr;
  start_ptr->prev = web_object;
  start_ptr = web_object;
}
/* error 표시 함수 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];
  
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor="
                "ffffff"
                ">\r\n",
          body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));

  Rio_writen(fd, body, strlen(body));
}