/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, void *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, void *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  /* 듣기 socket open */
  listenfd = Open_listenfd(argv[1]);
  /* 무한 loop 서버 */
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // line:netp:tiny:accept, 반복적으로 연결 요청 접수
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    /* doit은 트랜잭션을 수행한다는 뜻 */
    doit(connfd);  // line:netp:tiny:doit
    /* close는 트랜잭션을 수행한다는 뜻 */
    Close(connfd); // line:netp:tiny:close
  }
}

void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;
  /* Read request line and headers */
  Rio_readinitb(&rio, fd); // 여기서 fd가 지정되는 거?
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); // **** buf에 url 뒤 형식이 들어가는 거?
  if (!(strcasecmp(method, "GET") ==0 || strcasecmp(method, "HEAD")==0)) // GET 메서드만
  {
    /* GET 요청이 아닌 경우 */
    clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio); // 다른 요청 헤더들은 무시하기 위한 코드

  /* Parse URI from GET request */
  is_static = parse_uri(uri, filename, cgiargs);
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn’t find this file");
    return;
  }

  if (is_static)
  { /* Serve static content */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) // 파일의 권환을 갖고 있는지, **** 이런거 알아야 하냐
    {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn’t read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size, method);
  }
  else
  { /* Serve dynamic content */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) // 파일의 권한을 갖고 있는지
    {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn’t run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs, method);
  }
}
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];
  /* Build the HTTP response body */
  sprintf(body, "<html><title>Tiny Error</title>");
  sprintf(body, "%s<body bgcolor="
                "ffffff"
                ">\r\n",
          body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
}
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];
  Rio_readlineb(rp, buf, MAXLINE);
  while (strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  if (!strstr(uri, "cgi-bin"))
  { /* Static content */
    strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri);
    if (uri[strlen(uri) - 1] == '/')
      strcat(filename, "home.html"); // 기본 파일 주소 추가(home.html)
    return 1;
  }
  else
  { /* Dynamic content */
    ptr = index(uri, '?');
    if (ptr)
    {
      strcpy(cgiargs, ptr + 1);
      *ptr = '\0';
    }
    else
      strcpy(cgiargs, "");
    strcpy(filename, "."); // 상대 리눅스 파일 이름으로 변환
    strcat(filename, uri); // 상대 리눅스 파일 이름으로 변환
    return 0;
  }
}

void serve_static(int fd, char *filename, int filesize, void *method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];
  /* Send response headers to client */
  get_filetype(filename, filetype);                          // 파일 이름의 접미어 부분 검사
  sprintf(buf, "HTTP/1.0 200 OK\r\n");                       // 클라이언트에 응답 줄과 응답 헤더를 보냄
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);        //
  sprintf(buf, "%sConnection: close\r\n", buf);              //
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);   //
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype); // 하나의 빈 줄로 헤더를 종료(**** 여기서 RIO는 뭥미?)
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n");
  printf("%s", buf);

  if (strcasecmp(method, "HEAD") == 0)
    return;

  /* Send response body to client */
  srcfd = Open(filename, O_RDONLY, 0);                       // 읽기 위해 filename을 Open
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);// 오픈한 파일 가상메모리 영역으로 매핑(파일의 첫번째 filesize 바이트를 주소 srcp에서 시작하는 가상 메모리 영역으로 매핑)
  srcp = (void*)malloc(filesize);
  if (srcp == NULL){
    Close(srcfd);
    return;
  }

  Rio_readn(srcfd, srcp, filesize);
  Close(srcfd);                                              // 메모리 누수 방지(close)

  Rio_writen(fd, srcp, filesize);
  free(srcp);
  // Munmap(srcp, filesize);                                    // 메모리 누수 방지(매핑된 가상메모리 주소 반환)
}

/*
 * get_filetype - Derive file type from filename
 */
void get_filetype(char *filename, char *filetype)
{
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}
void serve_dynamic(int fd, char *filename, char *cgiargs, void *method)
{
  char buf[MAXLINE], *emptylist[] = {NULL};
  /* Return first part of HTTP response */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));
  
  /* 새로운 자식 프로세스 fork */
  if (Fork() == 0)
  { /* Child, 부모가 죽어서 서버가 함께 죽는 거 방지 위해 자식 포크 */
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1);
    Dup2(fd, STDOUT_FILENO);              // 자식의 표준 출력을 연결 파일 식별자로 재지정 후에
    Execve(filename, emptylist, environ); // CGI 프로그램을 재시작
  }
  Wait(NULL); /* Parent waits for and reaps child */
}
