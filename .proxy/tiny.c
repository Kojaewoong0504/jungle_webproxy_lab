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
void serve_static(int fd, char *filename, int filesize, int is_head);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

int main(int argc, char **argv)
{
  int listenfd, connfd; // listenfd: 서버가 클라이언트 연결을 기다리는 리스닝 소켓
                        // connfd: 수락된 클라이언트와 통신하는 전용 연결 소켓
  char hostname[MAXLINE], port[MAXLINE]; // hostname, port: 클라이언트 주소 정보 출력을 위한 문자열 버퍼
  socklen_t clientlen; // clientlen: accept() 에서 주소 크기 전달 및 갱신용
  struct sockaddr_storage clientaddr; // clientaddr: 클라이언트의 주소 정보 저장용 구조체

  /* Check command line args */
  if (argc != 2) // 포트 번호를 명령줄 인자로 하나 받지 않으면 메시지 출력 후 종료
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  /* 
    Open_listenfd()는 CSAPP에서 제공하는 에러 처리를 포함한 레퍼 함수
    주어진 포트 번호로 소켓 생성 + 바인딩 + 리스닝을 한 번에 수행
    반환된 listenfd는 클라이언트 연결 요청을 수락할 준비가 된 소켓
  */
  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr); // accept()에 넘길 주소 버퍼의 크기를 설정
    connfd = Accept(listenfd, (SA *)&clientaddr, // 클라이언트 연결 요청을 수락하고 전용 소켓(connfd)을 생성
                    &clientlen); // line:netp:tiny:accept
    /*
      clientaddr에 저장된 바이너리 주소를 문자열 형태로 변환
      hostname: 클라이언트 IP 또는 호스트명
      port: 클라이언트가 사용한 포트
      로그 및 디버깅용
    */
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port); // 클라이언트가 연결한 주소와 포트를 출력
    doit(connfd);  // 핵심 처리 함수 호출, 클라이언트가 보낸 HTTP 요청을 읽고 분석하여 적절한 응답을 생성
    Close(connfd); // connfd는 이 클라이언트 요청 하나만을 위해 열려 있던 소켓, 처리 끝나면 반드시 닫아야 함
  }
}

void doit(int fd){
  int is_static; // 요청이 정적 콘텐츠인지 동적 콘텐츠인지 구분
  int is_head = 0;
  struct stat sbuf; //파일의 메타데이터를 저장하기 위한 구조체 (특정 파일에 대한 다양한 정보)
  /*
    buf: 한 줄씩 읽는 버퍼
    method, uri, version: 요청 라인의 구성 요소 ex) GET /index.html HTTP/1.1 
  */
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE]; 
  char filename[MAXLINE], cgiargs[MAXLINE]; // 실제 서버가 처리할 파일 경로와 CGI 인자
  rio_t rio; //robust I/O 를 구현하기 위한 버퍼 기반의 I/O 상태를 담는 구조체

  Rio_readinitb(&rio, fd); // 클라이언트 소켓 fd를 rio 구조체와 연결
  Rio_readlineb(&rio, buf, MAXLINE); // 클라이언트가 보낸 첫 줄 요청 라인 (예: GET /index.html HTTP/1.1) 읽기
  printf("Request headers:\n"); // 디버깅 출력용
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); //요청 라인을 method, uri, version으로 분리
  if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "HEAD") != 0) { 
    // GET외 다른 HTTP 메소드는 지원하지 않는다. 대소문자 구분 없이 비교 strcasecmp
    clienterror(fd, method, "501", "NOT implemented", "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio); // 요청 헤더는 사용하지 않고 그냥 읽기만 한다.
  if (strcasecmp(method, "HEAD") == 0) {
    is_head = 1;
  }
  is_static = parse_uri(uri, filename, cgiargs); // URI 가 정적이면 filename만 동적이면 cgiargs까지 추출
  if (stat(filename, &sbuf) < 0){ // 실제로 해당 파일이 존재하는지 확인 실패하면 404 Not Fount 반환
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }

  if (is_static){ // 정적 콘텐츠 처리
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)){ // 정규 파일이고 읽기 원한이 있을 경우만 제공
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size, is_head); // 문제 없으면 serve_static() 호출
  }
  else{ // 동적 콘텐츠 처리
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)){ // 실행 권한이 있는 정규 파일이어야 CGI 프로그램 실행 가능
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs); // serve_dynamic() 호출
  }
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg){
  /*
    fd: 클라이언트와의 연결 소켓
    cause: 오류의 원인 문자열 (예: 파일 이름)
    errnum: HTTP 상태 코드 (예: "404")
    shortmsg: 짧은 설명 (예: "Not Found")
    longmsg: 사용자에게 보여줄 상세 설명 (예: "Tiny couldn't find this file")
  */
  char buf[MAXLINE], body[MAXLINE]; // buf: HTTP 헤더 문자열 저장용, body: 응답 본문 HTML 저장용

  /* 
    Build the HTTP response body 
    body에 간단한 HTML 문서를 한 줄씩 누적하여 작성
    이 본문은 나중에 클라이언트에게 전송됨
    에러 코드, 설명, 원인을 HTML로 포맷팅해서 사용자에게 보여줌
  */
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
  /*
    sprintf()는 문자열을 포맷팅해서 다른 문자열 버퍼에 저장하는 함수다. 
    printf()와 달리 화면에 출력하지 않고, 버퍼에 작성한다는 점이 다르다.
  */

  /*
    rio_writen
    리눅스 시스템 호출 write(fd, buf, n)은 항상 n바이트를 쓰는 것이 아니다
    시스템에 따라 버퍼 부족, 시그널, 인터럽트 등으로 부분만 쓸 수 있음 (short count)

    내부적으로 반복문을 통해 요청한 바이트 수만큼 모두 쓸 때까지 루프를 돈다. 쓰다가 중단돼도 계속 이어서 보냄
  */
}

void read_requesthdrs(rio_t *rp){ // rp: rio_t 구조체 포인터 (robust I/O 상태)
  char buf[MAXLINE]; // 한 줄씩 읽어들일 임시 버퍼

  rio_readlineb(rp, buf, MAXLINE); // 첫 번째 요청 헤더 라인을 읽는다.
  printf("%s", buf); //읽은 라인을 출력한다.
  while(strcmp(buf, "\r\n")){ // HTTP 헤더는 빈 줄 \r\n으로 끝난다. 즉 빈 줄이 나올때 까지 한줄씩 읽는다.
    rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  }
  return;
}

/*
  uri: 클라이언트가 보낸 요청 URI 예) /index.html, /cgi-bin/adder?1&2
  filename: URI에 대응하는 실제 파일 경로를 저장할 버퍼
  cgiargs: 동적 콘텐츠라면 인자 추출용
*/
int parse_uri(char *uri, char *filename, char *cgiargs){
  char *ptr; // ? 문자를 찾을 때 사용할 포인터

  if (!strstr(uri, "cgi-bin")){ // URI에 cgi-bin이 없으면 정적 콘텐츠다.
    strcpy(cgiargs, "");                 // CGI 인자 없음
    strcpy(filename, ".");               // 상대 경로 시작
    strcat(filename, uri);               // ./index.html 같은 경로 생성
    if (uri[strlen(uri)-1] == '/'){      // 디렉토리 요청이면
      strcat(filename, "home.html");     // 기본 파일로 home.html 사용
    }
    return 1;                            // 정적 콘텐츠임을 알림
  }
  else {
    ptr = index(uri, '?');
    if (ptr){
      strcpy(cgiargs, ptr+1);   // ? 뒤의 문자열을 인자로 저장
      *ptr = '\0';              // ?를 널 문자로 바꿔서 URI 자르기
    }
    else{
      strcpy(cgiargs, "");      // ? 없으면 인자 없음
    }
    strcpy(filename, ".");      // 파일 경로 시작
    strcat(filename, uri);      // ./cgi-bin/adder 같은 경로로 만듦
    return 0;                   // 동적 콘텐츠임을 알림
  }
}

/*
  fd: 클라이언트와 연결된 소켓 디스크립터
  filename: 제공할 파일 경로
  filesize: 파일 크기 (바이트 단위)
*/
void serve_static(int fd, char *filename, int filesize, int is_head){
  int srcfd; // 파일을 열 때 사용할 디스크립터
  /*
    srcp: 메모리에 매핑된 파일의 포인터
    filetype: MIME 타입 저장용 (예: text/html)
    buf: HTTP 응답 헤더 작성용 버퍼
  */
  char *srcp, filetype[MAXLINE], buf[MAXLINE];

  /* Send response headers to client */
  get_filetype(filename , filetype); //파일확장자를 보고 filetype 설정
  /*
    응답 상태줄: HTTP/1.0 200 OK
    서버 이름, 연결 상태, 콘텐츠 길이, 콘텐츠 타입
    마지막 \r\n\r\n으로 헤더 종료
  */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n",buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  Rio_writen(fd, buf, strlen(buf)); // 완성된 헤더를 클라이언트에 전송
  printf("Response headers:\n");
  printf("%s", buf); // 서버 측 로그 출력용
  if (is_head == 0){
    /* Send response body to client */
    srcfd = Open(filename, O_RDONLY, 0); // 파일 열기
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); //Mmap으로 파일을 메모리에 매핑 (읽기 전용, private)
    Close(srcfd); // 파일 디스크립터는 닫아도 매핑은 유지된다.
    Rio_writen(fd, srcp, filesize); // 매핑된 메모리 내용을 클라이언트에게 전송
    Munmap(srcp, filesize); // 매핑 해제하여 메모리 정리
  }

  /* 숙제 11.9 */
  // srcfd = Open(filename, O_RDONLY, 0);
  // if (srcfd < 0){
  //   clienterror(fd, filename, "404", "Not found", "Tiny couldn't open the file");
  //   return;
  // }
  // char *srcbuf = malloc(filesize);
  // if (!srcbuf){
  //   Close(srcfd);
  //   return;
  // }
  // rio_t rio;
  // Rio_readinitb(&rio, srcfd);
  // Rio_readn(srcfd, srcbuf, filesize);

  // Rio_writen(fd, srcbuf, filesize);

  // free(srcbuf);
  // Close(srcfd);
}


/*
  filename: 클라이언트가 요청한 파일 이름 예) ./index/.html
  filetype: MIME 타입을 저장할 문자열 버퍼
*/
void get_filetype(char *filename, char *filetype){  
  if (strstr(filename, ".html")){ //strstr은 전체 문자열에서 찾을 부분 문자열이 처음 등장하는 위치를 반환한다.
    strcpy(filetype, "text/html");
  }
  else if (strstr(filename, ".gif")){
    strcpy(filetype, "image/gif");
  }
  else if (strstr(filename, ".png")){
    strcpy(filetype, "image/png");
  }
  else if (strstr(filename, ".jpg")){
    strcpy(filetype, "image/jpeg");
  }
  else if (strstr(filename, ".mpg")){
    strcpy(filetype, "video/mpeg");
  }
  else{
    strcpy(filetype, "text/plain");
  }

}

/*
  fd: 클라이언트 소켓 디스크립터
  filename: 실행할 CGI 프로그램 파일명
  cgiargs: 클라이언트가 보낸 CGI 인자 (예: ?a=1&b=2)
*/
void serve_dynamic(int fd, char *filename, char *cgiargs){
  /*
    buf: 응답 헤더용 임시 버퍼
    emptylist: 프로그램 인자 없음 (argv 전달용)
  */
  char buf[MAXLINE], *emptylist[] = { NULL };

  /* Return first part of HTTP response */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  if (Fork() == 0) { // 자식 프로세스 생성 CGI 프로그램은 자식 프로세스에서 실행
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); // cgiargs를 환경 변후 "QUERY_STRING"에 저장 
                                        // CGI 프로그램은 이 값을 통해 인자를 읽는다.
    Dup2(fd, STDOUT_FILENO); // stdout을 클라이언트 소켓으로 바꿈 CGI 프로그램의 출력이 클라이언트로 바로 전송됨
    /*
      CGI 프로그램 실행
      filename: 실행할 경로
      emptylist: 인자 없음
      environ: 환경 변수 전달
    */
    Execve(filename, emptylist, environ);
  }
  Wait(NULL); // 부모는 자식이 종료할 때까지 기다린다. CGI 실행 완료 후 정리
}