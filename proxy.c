#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* 캐시 구현 */
typedef struct cache_block {
  char uri[MAXLINE];
  char *object;
  int size;
  struct cache_block *prev, *next;
} cache_block_t;

typedef struct {
  cache_block_t *head;
  cache_block_t *tail;
  int total_size;
  pthread_rwlock_t lock;
} cache_list_t;

void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void read_requesthdrs(rio_t *rp, char *host_header, char *other_header);
void parse_uri(const char *uri, char *hostname, char *port, char *path);
void reassemble(char *req, char *path, char *hostname, char *other_header);
// void forward_response(int servedf, int fd);
void *thread(void *vargp);
void cache_init();
char *cache_find(const char *uri, int *size_out);
void cache_insert(const char *uri, const char *buf, int size);

cache_list_t cache;

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
  cache_init();

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

void cache_init(){
  cache.head = NULL;
  cache.tail = NULL;
  cache.total_size = 0;
  pthread_rwlock_init(&cache.lock, NULL);
}

char *cache_find(const char *uri, int *size_out) {
  cache_block_t *p;

  // 1. 읽기 락 먼저 획득
  pthread_rwlock_rdlock(&cache.lock);

  // 2. URI를 찾기
  p = cache.head;
  while (p) {
    if (strcmp(p->uri, uri) == 0) {
      break;
    }
    p = p->next;
  }

  // 3. 못 찾았으면 종료
  if (!p) {
    pthread_rwlock_unlock(&cache.lock);
    return NULL;
  }

  // 4. 찾았지만 LRU 갱신은 쓰기 작업 → 락 승격 필요
  pthread_rwlock_unlock(&cache.lock);       // 읽기 락 해제
  pthread_rwlock_wrlock(&cache.lock);       // 쓰기 락 획득

  // 5. 다시 찾아야 함 (잠금 사이에 다른 스레드가 변경했을 수 있음)
  p = cache.head;
  while (p) {
    if (strcmp(p->uri, uri) == 0) {
      break;
    }
    p = p->next;
  }

  if (!p) {
    pthread_rwlock_unlock(&cache.lock);
    return NULL;
  }

  // 6. LRU 업데이트
  if (p != cache.head) {
    if (p->prev) p->prev->next = p->next;
    if (p->next) p->next->prev = p->prev;
    if (p == cache.tail) cache.tail = p->prev;

    p->next = cache.head;
    p->prev = NULL;
    if (cache.head) cache.head->prev = p;
    cache.head = p;
  }

  // 7. 캐시 데이터 복사해서 반환 (락 해제 후에도 유효해야 하므로)
  char *copy = malloc(p->size);
  memcpy(copy, p->object, p->size);
  *size_out = p->size;

  pthread_rwlock_unlock(&cache.lock);
  return copy;
}


// 새 응답 객체를 캐시에 저장. 용량 초과 시 LRU 제거
void cache_insert(const char *uri, const char *buf, int size){
  if (size > MAX_OBJECT_SIZE) return;

  pthread_rwlock_wrlock(&cache.lock); // 🔒 WRLOCK (쓰기 단독 허용)

  // LRU 제거: 필요 시 용량 확보
  while (cache.total_size + size > MAX_CACHE_SIZE){
    cache_block_t *old = cache.tail;
    if (old == NULL) break;

    cache.tail = old->prev;
    if (cache.tail) {
      cache.tail->next = NULL;
    } else {
      cache.head = NULL;
    }

    cache.total_size -= old->size;
    free(old->object);
    free(old);
  }

  // 새 블록 생성
  cache_block_t *new_block = malloc(sizeof(cache_block_t));
  strcpy(new_block->uri, uri);
  new_block->object = malloc(size);
  memcpy(new_block->object, buf, size);
  new_block->size = size;

  // 새 블록을 head에 삽입
  new_block->prev = NULL;
  new_block->next = cache.head;
  if (cache.head) cache.head->prev = new_block;
  cache.head = new_block;
  if (cache.tail == NULL) cache.tail = new_block;

  cache.total_size += size;

  pthread_rwlock_unlock(&cache.lock); // 🔓 UNLOCK
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

  /* 캐시 */
  int cached_size;
  char *cached_obj = cache_find(uri, &cached_size);
  if (cached_obj) {
      Rio_writen(fd, cached_obj, cached_size);
      free(cached_obj); // 복사본이므로 사용 후 해제해야 메모리 누수 방지
      return;
  }  

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
  // forward_response(servedf, fd);

  // 🔽 forward_response() 대신 직접 읽고 캐싱
  rio_t serve_rio;
  char response_buf[MAXLINE];
  char obj_buf[MAX_OBJECT_SIZE];
  int total = 0;
  ssize_t n;

  Rio_readinitb(&serve_rio, servedf);
  while ((n = Rio_readlineb(&serve_rio, response_buf, MAXLINE)) > 0){
      if (total + n <= MAX_OBJECT_SIZE)
          memcpy(obj_buf + total, response_buf, n);
      total += n;
      Rio_writen(fd, response_buf, n);
  }
  Close(servedf);

  if (total <= MAX_OBJECT_SIZE)
      cache_insert(uri, obj_buf, total);

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

// void forward_response(int servedf, int fd){
//   rio_t serve_rio;
//   char response_buf[MAXLINE];

//   Rio_readinitb(&serve_rio, servedf);
//   ssize_t n;
//   while ((n = Rio_readlineb(&serve_rio, response_buf, MAXLINE)) > 0){
//     Rio_writen(fd, response_buf, n);
//   }
  
// }

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
