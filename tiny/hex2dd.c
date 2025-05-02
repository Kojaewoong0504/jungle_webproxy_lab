#include "csapp.h"

int main(int argc, char **argv) {
    struct in_addr inaddr;       // 네트워크 바이트 순서의 주소
    uint16_t addr;               // 호스트 바이트 순서 주소
    char buf[MAXBUF];           // 문자열로 변환된 주소 저장 버퍼

    if (argc != 2) {
        fprintf(stderr, "usage: %s <hex number>\n", argv[0]);
        exit(0);
    }

    sscanf(argv[1], "%hx", &addr);          // 입력 문자열을 16진수 정수로 파싱
    inaddr.s_addr = htons(addr);           // 호스트 바이트 순서를 네트워크 바이트 순서로 변환

    if (!inet_ntop(AF_INET, &inaddr, buf, MAXBUF)) {
        unix_error("inet_ntop");
    }

    printf("%s\n", buf);                   // 점 표기 문자열로 출력

    exit(0);
}
