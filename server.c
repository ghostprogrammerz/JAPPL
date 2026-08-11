#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "parser.h"
#include "emitter.h"

#define DEFAULT_PORT 8765
#define MAX_REQUEST (4 * 1024 * 1024)
#define TMPDIR "/tmp"

static char g_static_dir[4096];
static char g_ide_html[4096];
static char g_ide_dir[4096];
static char *g_sb3_buf = NULL;
static size_t g_sb3_len = 0;
static char *g_preview_buf = NULL;
static size_t g_preview_len = 0;

static void send_all(int fd, const char *buf, size_t len) {
    while (len) { ssize_t n = write(fd, buf, len); if (n <= 0) return; buf += n; len -= (size_t)n; }
}
static void send_str(int fd, const char *s) { send_all(fd, s, strlen(s)); }
static void http_respond(int fd, int status, const char *type, const char *body, size_t len) {
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Internal Server Error";
    char h[768];
    snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, reason, type, len);
    send_str(fd, h); if (body && len) send_all(fd, body, len);
}
static char *read_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f); if (sz <= 0) { fclose(f); return NULL; }
    rewind(f); char *b = malloc((size_t)sz); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *out_len = (size_t)sz; return b;
}

typedef struct { char method[8]; char path[256]; size_t content_length; char *body; size_t body_len; } Request;
static int read_request(int fd, Request *r) {
    memset(r, 0, sizeof(*r)); size_t cap=8192, hlen=0, header_end=0; char *h=malloc(cap); if(!h)return -1;
    for(;;){ if(hlen>=cap){cap*=2;if(cap>MAX_REQUEST){free(h);return -1;}char*t=realloc(h,cap);if(!t){free(h);return -1;}h=t;} ssize_t n=read(fd,h+hlen,cap-hlen);if(n<=0){free(h);return -1;} hlen+=(size_t)n; for(size_t i=hlen-(size_t)n>=3?hlen-(size_t)n-3:0;i+3<hlen;i++) if(h[i]=='\r'&&h[i+1]=='\n'&&h[i+2]=='\r'&&h[i+3]=='\n'){header_end=i+4;goto headers_done;} }
headers_done:
    char *p=h,*s1=memchr(p,' ',header_end);if(!s1){free(h);return -1;}size_t ml=(size_t)(s1-p);if(ml>=sizeof(r->method)){free(h);return -1;}memcpy(r->method,p,ml);r->method[ml]=0;
    p=s1+1;char*s2=memchr(p,' ',header_end-(size_t)(p-h));if(!s2){free(h);return -1;}size_t pl=(size_t)(s2-p);if(pl>=sizeof(r->path))pl=sizeof(r->path)-1;memcpy(r->path,p,pl);r->path[pl]=0;
    r->content_length=0;char*line=memchr(h,'\n',header_end);while(line){line++;if(strncasecmp(line,"content-length:",15)==0){r->content_length=(size_t)atoll(line+15);break;}if((size_t)(line-h)>=header_end)break;line=memchr(line,'\n',header_end-(size_t)(line-h));}
    size_t total=r->content_length,already=hlen-header_end;if(total>MAX_REQUEST){free(h);return -1;}r->body=malloc(total+1);if(!r->body){free(h);return -1;}if(already>total)already=total;if(already)memcpy(r->body,h+header_end,already);free(h);size_t got=already;while(got<total){ssize_t n=read(fd,r->body+got,total-got);if(n<=0){free(r->body);r->body=NULL;return -1;}got+=(size_t)n;}r->body[total]=0;r->body_len=total;return 0;
}
static const char *mime_for(const char *p){const char*d=strrchr(p,'.');if(!d)return"application/octet-stream";if(!strcmp(d,".html"))return"text/html; charset=utf-8";if(!strcmp(d,".js"))return"application/javascript; charset=utf-8";if(!strcmp(d,".css"))return"text/css; charset=utf-8";return"application/octet-stream";}

/* Inject a tiny local controller into the IDE. It never contacts turbowarp.org. */
static void handle_static(int fd,const char *url){
    char path[4096];
    if(!strcmp(url,"/")||!strcmp(url,"/index.html")){
        size_t hl=0;char*html=read_file_binary(g_ide_html,&hl);if(!html){http_respond(fd,404,"text/plain","Not Found\n",10);return;}
        const char marker[]="</body>";size_t off=0;for(size_t i=hl;i>=sizeof(marker)-1;i--){if(!memcmp(html+i-(sizeof(marker)-1),marker,sizeof(marker)-1)){off=i-(sizeof(marker)-1);break;}if(i==sizeof(marker)-1)break;}
        const char script[]="<script>(function(){function frame(){return document.getElementById('tw-iframe');}window.loadProject=function(){var f=frame();var p=document.getElementById('stage-placeholder');var l=document.getElementById('stage-label');var b=document.getElementById('backend-url').value.trim();if(!f)return;if(p)p.style.display='none';f.style.display='block';if(l)l.textContent='Loading…';f.onload=function(){if(l)l.textContent='Running';if(typeof setStatus==='function')setStatus('running','ok')};f.onerror=function(){if(l)l.textContent='Preview failed';if(typeof setStatus==='function')setStatus('preview failed','err')};f.src=b+'/preview?ts='+Date.now()};window.greenFlag=function(){var f=frame();if(!f||f.style.display==='none'){if(typeof compileAndRun==='function')compileAndRun();return}f.src=f.src.split('?')[0]+'?ts='+Date.now()};window.stopAll=function(){var f=frame();if(!f)return;f.src='about:blank';f.style.display='none';var p=document.getElementById('stage-placeholder');var l=document.getElementById('stage-label');if(p)p.style.display='flex';if(l)l.textContent='Stopped';if(typeof setStatus==='function')setStatus('stopped','')};window.fullscreen=function(){var f=frame();if(f&&f.requestFullscreen)f.requestFullscreen()}})();</script>";
        if(off){size_t sl=sizeof(script)-1,outlen=hl+sl;char*out=malloc(outlen);if(!out){free(html);http_respond(fd,500,"text/plain","Out of memory\n",14);return;}memcpy(out,html,off);memcpy(out+off,script,sl);memcpy(out+off+sl,html+off,hl-off);http_respond(fd,200,"text/html; charset=utf-8",out,outlen);free(out);}else http_respond(fd,200,"text/html; charset=utf-8",html,hl);free(html);return;
    }
    if(!strncmp(url,"/static/",8)||!strncmp(url,"/chunks/",8)){const char*slash=strchr(url+1,'/');const char*file=slash?slash+1:NULL;const char*sub=url[1]=='s'?"static":"chunks";if(!file||strstr(file,"..")||strchr(file,'/')){http_respond(fd,403,"text/plain","Forbidden\n",10);return;}snprintf(path,sizeof(path),"%s/%s/%s",g_static_dir,sub,file);}else{http_respond(fd,404,"text/plain","Not Found\n",10);return;}
    size_t len=0;char*b=read_file_binary(path,&len);if(!b){http_respond(fd,404,"text/plain","Not Found\n",10);return;}http_respond(fd,200,mime_for(path),b,len);free(b);
}

static int package_sb3(const char *sb3,char **out,size_t *outlen){
    char html[256],cmd[8192];snprintf(html,sizeof(html),TMPDIR "/jappl_%d_preview.html",(int)getpid());snprintf(cmd,sizeof(cmd),"node \"%s/package-runtime.js\" \"%s\" \"%s\"",g_ide_dir,sb3,html);int rc=system(cmd);if(rc!=0){fprintf(stderr,"TurboWarp Packager failed. Run: cd ide && npm install\n");unlink(html);return -1;}*out=read_file_binary(html,outlen);unlink(html);return *out?0:-1;
}
static void handle_compile(int fd,const char*src,size_t src_len){(void)src_len;char tmp[128];snprintf(tmp,sizeof(tmp),TMPDIR "/jappl_%d.sb3",(int)getpid());Parser p;parser_init(&p,src);Program*prog=parser_parse(&p);if(p.errors>0){const char*m="Compilation failed — check server stderr for details.\n";http_respond(fd,400,"text/plain",m,strlen(m));return;}if(emit_sb3(prog,tmp)!=0){const char*m="Emitter failed to write .sb3\n";http_respond(fd,500,"text/plain",m,strlen(m));return;}size_t len=0;char*sb3=read_file_binary(tmp,&len);if(!sb3){unlink(tmp);http_respond(fd,500,"text/plain","Failed to read compiled .sb3\n",29);return;}char*preview=NULL;size_t plen=0;if(package_sb3(tmp,&preview,&plen)==0){free(g_preview_buf);g_preview_buf=preview;g_preview_len=plen;}else{free(g_preview_buf);g_preview_buf=NULL;g_preview_len=0;}unlink(tmp);free(g_sb3_buf);g_sb3_buf=sb3;g_sb3_len=len;http_respond(fd,200,"application/zip",g_sb3_buf,g_sb3_len);}
static void handle_connection(int fd){Request r;if(read_request(fd,&r)!=0){close(fd);return;}if(!strcmp(r.method,"OPTIONS")){http_respond(fd,200,"text/plain","",0);goto done;}if(!strcmp(r.path,"/")||!strcmp(r.path,"/index.html")||!strncmp(r.path,"/static/",8)||!strncmp(r.path,"/chunks/",8)){handle_static(fd,r.path);goto done;}if(!strncmp(r.path,"/preview",8)){if(g_preview_buf)http_respond(fd,200,"text/html; charset=utf-8",g_preview_buf,g_preview_len);else{const char*m="No packaged project yet. Compile first and run npm install in ide/.\n";http_respond(fd,404,"text/plain",m,strlen(m));}goto done;}if(!strcmp(r.path,"/download")){if(g_sb3_buf)http_respond(fd,200,"application/zip",g_sb3_buf,g_sb3_len);else http_respond(fd,404,"text/plain","No project compiled yet\n",25);goto done;}if(!strcmp(r.path,"/compile")){if(strcmp(r.method,"POST")){http_respond(fd,405,"text/plain","Method Not Allowed\n",19);goto done;}if(!r.body||!r.body_len){http_respond(fd,400,"text/plain","Empty body\n",11);goto done;}handle_compile(fd,r.body,r.body_len);goto done;}http_respond(fd,404,"text/plain","Not Found\n",10);done:free(r.body);close(fd);}
int main(int argc,char**argv){int port=argc>=2?atoi(argv[1]):DEFAULT_PORT;char exe[4096]={0};ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n>0){exe[n]=0;char*s=strrchr(exe,'/');if(s)*s=0;snprintf(g_ide_dir,sizeof(g_ide_dir),"%s/ide",exe);snprintf(g_static_dir,sizeof(g_static_dir),"%s/ide/static",exe);snprintf(g_ide_html,sizeof(g_ide_html),"%s/ide/index.html",exe);}else{snprintf(g_ide_dir,sizeof(g_ide_dir),"ide");snprintf(g_static_dir,sizeof(g_static_dir),"ide/static");snprintf(g_ide_html,sizeof(g_ide_html),"ide/index.html");}int srv=socket(AF_INET,SOCK_STREAM,0);if(srv<0){perror("socket");return 1;}int opt=1;setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};if(bind(srv,(struct sockaddr*)&a,sizeof(a))<0){perror("bind");return 1;}if(listen(srv,16)<0){perror("listen");return 1;}printf("jappl2sb3 server listening on http://0.0.0.0:%d\n",port);fflush(stdout);for(;;){struct sockaddr_in ca;socklen_t cl=sizeof(ca);int fd=accept(srv,(struct sockaddr*)&ca,&cl);if(fd<0){if(errno==EINTR)continue;perror("accept");continue;}handle_connection(fd);} }
