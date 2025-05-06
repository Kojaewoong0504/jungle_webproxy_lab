#include <stdio.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define HASH_SIZE 997  // prime number to avoid collision


/* Cache structures */
typedef struct cache_block{
  char uri[MAXLINE];
  char *object;
  int size;
  struct cache_block *prev;
  struct cache_block *next;
  struct cache_block *hnext;
}cache_block_t;

typedef struct{
  cache_block_t *head;
  cache_block_t *tail;
  int total_size;
  pthread_rwlock_t lock;
  cache_block_t *hash_table[HASH_SIZE];
}cache_list_t;

cache_list_t cache;


void doit(int fd);
void read_requesthdrs(rio_t *rp, char *host_header, char *other_header);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *hostname, char *port, char *paht);
void reassemble(char *req, char *path, char *hostname, char *other_header);
void *thread(void *vargp);
void cache_init();
void cache_move_to_end(cache_list_t *cache, cache_block_t *node);
void cache_evict(cache_list_t *cache, int size_needed);
void cache_insert(cache_list_t *cache, const char *uri, const char *object, int size);
int cache_find(cache_list_t *cache, const char *uri, char *object_buf, int *size_buf);
unsigned int hash_uri(const char *uri);


 /* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
  "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
  "Firefox/10.0.3\r\n";
 
int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if (argc != 2){
      fprintf(stderr, "usage: %s <port>\n", argv[0]);
      exit(1);
    }
    
    listenfd = Open_listenfd(argv[1]);
    cache_init();
    while(1){
      clientlen = sizeof(clientaddr);
      connfd = Accept(listenfd,(SA *)&clientaddr, &clientlen);
      Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
      printf("Accepted connection from (%s, %s)\n", hostname, port);

      int *connfdp = malloc(sizeof(int));
      *connfdp = connfd;
      pthread_t tid;
      pthread_create(&tid, NULL, thread, connfdp);

    }
 }

void cache_init(){
  cache.head = NULL;
  cache.tail = NULL;
  cache.total_size = 0;
  pthread_rwlock_init(&cache.lock, NULL);
}

unsigned int hash_uri(const char *uri){
  unsigned int hash = 5381;
  int c;
  while ((c = *uri++)){
    hash = ((hash << 5) + hash) + c;
  }
  return hash % HASH_SIZE;
}


void cache_move_to_end(cache_list_t *cache, cache_block_t *node){
  if (cache->tail == node) return;

  if (node->prev){
    node->prev->next = node->next;
  } else{
    cache->head = node->next;
  }

  if (node->next){
    node->next->prev = node->prev;
  }else{
    cache->tail = node->prev;
  }

  node->prev = cache->tail;
  node->next = NULL;

  if (cache->tail){
    cache->tail->next = node;
  }else{
    cache->head = node;
  }

  cache->tail = node;
}

void cache_evict(cache_list_t *cache, int size_needed){
  while (cache->total_size + size_needed > MAX_CACHE_SIZE){
    if (cache->head == NULL) return;
    cache_block_t *oldest = cache->head;

    cache->head = oldest->next;
    if (cache->head){
      cache->head->prev = NULL;
    }else{
      cache->tail = NULL;
    }

    unsigned int idx = hash_uri(oldest->uri);
    cache_block_t **p = &cache->hash_table[idx];
    while (*p && *p != oldest){
      p = &(*p)->hnext;
    }
    if (*p){
      *p = (*p)->hnext;
    }

    cache->total_size -= oldest->size;

    free(oldest->object);
    free(oldest);
  }
}


void cache_insert(cache_list_t *cache, const char *uri, const char *object, int size){
  if (size > MAX_OBJECT_SIZE){
    return;
  }

  pthread_rwlock_wrlock(&cache->lock);

  cache_evict(cache, size);

  cache_block_t *new_block = malloc(sizeof(cache_block_t));
  if (!new_block){
    pthread_rwlock_unlock(&cache->lock);
    return;
  }

  strncpy(new_block->uri, uri, MAXLINE - 1);
  new_block->uri[MAXLINE - 1] = '\0';

  new_block->object = malloc(size);
  if (!new_block->object){
    free(new_block);
    pthread_rwlock_unlock(&cache->lock);
    return;
  }

  memcpy(new_block->object, object, size);
  new_block->size = size;

  new_block->prev = cache->tail;
  new_block->next = NULL;

  if (cache->tail){
    cache->tail->next = new_block;
  } else{
    cache->head = new_block;
  }
  cache->tail = new_block;
  cache->total_size += size;

  unsigned int idx = hash_uri(uri);
  new_block->hnext = cache->hash_table[idx];
  cache->hash_table[idx] = new_block;
  
  pthread_rwlock_unlock(&cache->lock);
}

int cache_find(cache_list_t *cache, const char *uri, char *object_buf, int *size_buf){
  pthread_rwlock_rdlock(&cache->lock);
  unsigned int idx = hash_uri(uri);
  cache_block_t *node = cache->hash_table[idx];
  while (node){
    if(strcmp(node->uri, uri) == 0){
      pthread_rwlock_unlock(&cache->lock);
      pthread_rwlock_wrlock(&cache->lock);

      cache_move_to_end(cache, node);
      memcpy(object_buf, node->object, node->size);
      *size_buf = node->size;

      pthread_rwlock_unlock(&cache->lock);
      return 1;
    }
    node = node->next;
  }

  pthread_rwlock_unlock(&cache->lock);
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
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char host_header[MAXLINE], other_header[MAXLINE];
  char hostname[MAXLINE], port[MAXLINE], path[MAXLINE];
  char request_buf[MAXLINE];
  rio_t rio;

  Rio_readinitb(&rio, fd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers:\n%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);

  if (strcasecmp(method, "GET") != 0){
    clienterror(fd, method, "501", "Not implemented", "This Server does not implement this method");
    return;
  }

  read_requesthdrs(&rio, host_header, other_header);
  parse_uri(uri, hostname, port, path);

  /* 캐시 관련 함수 추가 */
  char cache_buf[MAX_OBJECT_SIZE];
  int cache_size;
  if (cache_find(&cache, uri, cache_buf, &cache_size)) {
    Rio_writen(fd, cache_buf, cache_size);
    return;
  }

  int servedf = Open_clientfd(hostname, port);
  if (servedf < 0) {
    clienterror(fd, hostname, "404", "Not found", "Couldn't connect to server");
    return;
  }

  reassemble(request_buf, path, hostname, other_header);
  Rio_writen(servedf, request_buf, strlen(request_buf));

  char response_buf[MAXBUF];
  char temp_cache[MAX_OBJECT_SIZE];
  int total_size = 0;
  rio_t server_rio;
  Rio_readinitb(&server_rio, servedf);
  ssize_t n;

  while ((n = Rio_readnb(&server_rio, response_buf, MAXBUF)) > 0) {
    Rio_writen(fd, response_buf, n);
    if (total_size + n <= MAX_OBJECT_SIZE) {
      memcpy(temp_cache + total_size, response_buf, n);
    }
    total_size += n;
  }
  Close(servedf);

  if (total_size <= MAX_OBJECT_SIZE) {
    cache_insert(&cache, uri, temp_cache, total_size);
  }
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
  char response_buf[MAXBUF];

  Rio_readinitb(&serve_rio, servedf);
  ssize_t n;
  while ((n = Rio_readnb(&serve_rio, response_buf, MAXBUF)) > 0) {
    Rio_writen(fd, response_buf, n);
  }  
}


void read_requesthdrs(rio_t *rp, char *host_header, char *other_header){
  char buf[MAXLINE];
  host_header[0] = '\0';
  other_header[0] = '\0'; 

  while(Rio_readlineb(rp, buf, MAXLINE) > 0 && strcmp(buf, "\r\n")){
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

void parse_uri(char *uri, char *hostname, char *port, char *path){
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