#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void read_requesthdrs(rio_t *rp, char *host_header, char *other_header);
void parse_uri(const char *uri, char *hostname, char *port, char *path);
void reassemble(char *req, char *path, char *hostname, char *other_header);
void forward_response(int servedf, int fd);
void *thread(void *vargp);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

int main(int argc, char **argv) {
  int listenfd, connfd, clientfd;
  char client_hostname[MAXLINE], client_port[MAXLINE];
  
  socklen_t clientlen, serverlen;
  struct sockaddr_storage clientaddr, serveraddr;
  if (argc != 2) // 포트 번호를 명령줄 인자로 하나 받지 않으면 메시지 출력 후 종료
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  listenfd = Open_listenfd(argv[1]);
  while(1){
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    Getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", client_hostname, client_port);

    int *connfdp = malloc(sizeof(int));
    *connfdp = connfd;
    pthread_t tid;
    pthread_create(&tid, NULL, thread, connfdp);
  }
  printf("%s", user_agent_hdr);
  return 0;
}


void *thread(void *vargp){
  int connfd = *((int *)vargp);
  free(vargp);

  pthread_detach(pthread_self());

  doit(connfd);
  Close(connfd);

  return NULL;
}


void doit(int fd){
  /*
    buf: 한 줄씩 읽는 버퍼
    method, uri, version: 요청 라인의 구성 요소 ex) GET /index.html HTTP/1.1 
  */
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; 
  char hosthdr[MAXLINE], otherhdr[MAXLINE];
  char hostname[MAXLINE], port[MAXLINE], path[MAXLINE];
  char reqest_buf[MAXLINE];
  rio_t rio; //robust I/O 를 구현하기 위한 버퍼 기반의 I/O 상태를 담는 구조체

  Rio_readinitb(&rio, fd); // 클라이언트 소켓 fd를 rio 구조체와 연결
  Rio_readlineb(&rio, buf, MAXLINE); // 클라이언트가 보낸 첫 줄 요청 라인 (예: GET /index.html HTTP/1.1) 읽기
  printf("Request headers:\n"); // 디버깅 출력용
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); //요청 라인을 method, uri, version으로 분리
  if (strcasecmp(method, "GET") != 0) { 
    // GET외 다른 HTTP 메소드는 지원하지 않는다. 대소문자 구분 없이 비교 strcasecmp
    clienterror(fd, method, "501", "NOT implemented", "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio, hosthdr, otherhdr); // 요청 헤더는 사용하지 않고 그냥 읽기만 한다.
  parse_uri(uri, hostname, port, path);
  int servedf = Open_clientfd(hostname, port);
  reassemble(reqest_buf, path, hostname, otherhdr);
  Rio_writen(servedf, reqest_buf, strlen(reqest_buf));
  forward_response(servedf, fd);
}

void reassemble(char *req, char *path, char *hostname, char *other_header){
  sprintf(req,
    "GET %s HTTP/1.0\r\n"
    "Host: %s\r\n"
    "%s"
    "Connection: close\r\n"
    "Proxy-Connection: close\r\n"
    "%s"
    "\r\n",
    path,
    hostname,
    user_agent_hdr,
    other_header
  );
}

void forward_response(int servedf, int fd){
  rio_t serve_rio;
  char response_buf[MAXLINE];

  Rio_readinitb(&serve_rio, servedf);
  ssize_t n;
  while ((n = Rio_readlineb(&serve_rio, response_buf, MAXLINE)) > 0){
    Rio_writen(fd, response_buf, n);
  }
}

void read_requesthdrs(rio_t *rp, char *host_header, char *other_header){ // rp: rio_t 구조체 포인터 (robust I/O 상태)
  char buf[MAXLINE]; // 한 줄씩 읽어들일 임시 버퍼
  host_header[0] = '\0';
  other_header[0] = '\0';

  while(rio_readlineb(rp, buf, MAXLINE)>0 && strcmp(buf, "\r\n")){
    if (!strncasecmp(buf, "Host:", 5)){
      strcpy(host_header, buf);
    }
    else if (!strncasecmp(buf, "User-Agent:", 11) || !strncasecmp(buf, "Connection:", 11) || !strncasecmp(buf, "Proxy-Connection:", 17)) {
        continue;  // 무시
    }
    else{
      strcat(other_header, buf);
    }
  }
}

void parse_uri(const char *uri, char *hostname, char *port, char *path){
  char *hostbegin, *hostend, *portbegin, *pathbegin;
  char buf[MAXLINE];

  strcpy(buf, uri);

  hostbegin = strstr(buf, "//");
  hostbegin = (hostbegin != NULL) ? hostbegin + 2 : buf; 

  pathbegin = strchr(hostbegin, '/');
  if (pathbegin != NULL){
    strcpy(path, pathbegin);
    *pathbegin = '\0';
  }
  else{
    strcpy(path, "/");
  }

  portbegin = strchr(hostbegin, ':');
  if (portbegin != NULL) {
      *portbegin = '\0';                
      strcpy(hostname, hostbegin);
      strcpy(port, portbegin + 1);      
  } else {
      strcpy(hostname, hostbegin);
      strcpy(port, "80");       
  }
}


void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg){
  char buf[MAXLINE], body[MAXLINE]; // buf: HTTP 헤더 문자열 저장용, body: 응답 본문 HTML 저장용
  sprintf(body, "<html><title>Tiny Error</title></html>");
  sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
  sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
  sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
  sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n</body>", body);

  /* Print the HTTP response */
  sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
  Rio_writen(fd, buf, strlen(buf)); // 상태줄 전송 예: HTTP/1.0 404 Not Found
  sprintf(buf, "Content-type: text/html\r\n");
  Rio_writen(fd, buf, strlen(buf)); // MIME 타입 명시: HTML이라는 것을 알려줌
  sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body)); 
  Rio_writen(fd, buf, strlen(buf)); // 본문 길이 알려줌 + 빈 줄로 헤더 종료
  Rio_writen(fd, body, strlen(body)); // 위에서 만든 HTML을 클라이언트에게 전송
}
