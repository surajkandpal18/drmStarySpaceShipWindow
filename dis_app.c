#define _GNU_SOURCE
#include <termios.h>
#include<time.h>
#include<errno.h>
#include<fcntl.h>
#include<stdbool.h>
#include<stdint.h>
#include<stdio.h>
#include<ncurses.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<sys/mman.h>
#include<xf86drm.h>
#include<xf86drmMode.h>

#define max(a, b) ((a) > (b) ? (a) : (b))

static struct termios old_tio, new_tio;
static int arr_size=300;
struct modes_buf;
struct modes_device;
static bool game_over=false;
int x[300],y[300],m;
static enum direction {UP,DOWN,LEFT,RIGHT};
static enum direction dir=RIGHT;
static int modes_find_crtc(int fd,drmModeRes *res,drmModeConnector *conn,struct modes_device *device);
static int modes_create_fb(int fd,struct modes_buf *buf);
static void modes_destroy_fb(int fd,struct modes_buf *buf);
static int modes_setup_device(int fd,drmModeRes *res,drmModeConnector *conn,struct modes_device *device);
static int modes_open(int *out,const char *node);
static int modes_prepare(int fd);
static void modes_draw(int fd);
static void modes_draw_device(int fd, struct modes_device *device);

static void modes_cleanup(int fd);


static int modes_open(int *out,const char *node){
	int fd,ret;
	uint64_t has_dumb;
	fd=open(node,O_RDWR|O_CLOEXEC);
	if(fd<0){
		ret=-errno;
		fprintf(stderr,"cannot open '%s': %m\n",node);
		return ret;
	}

	if(drmGetCap(fd,DRM_CAP_DUMB_BUFFER,&has_dumb)<0||!has_dumb){
		fprintf(stderr,"drm deviceice '%s' does not support dumb buffer\n",node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out=fd;
	return 0;
}

struct modes_buf{
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};



struct modes_device{
	struct modes_device *next;

	unsigned int front_buf;
	struct modes_buf bufs[2];
	drmModeModeInfo mode;

	uint32_t conn;
	uint32_t crtc;
	drmModeCrtc *saved_crtc;
	bool pflip_pending;
	bool cleanup;

	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};

static struct modes_device *modes_list=NULL;

static int modes_prepare(int fd){
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modes_device *device;
	int ret;

	res=drmModeGetResources(fd);
	if(!res){
		fprintf(stderr,"cannot retrieve DRM resources (%d) : %m\n",errno);
		return -errno;
	}

	for(i=0;i<res->count_connectors;++i){
		conn=drmModeGetConnector(fd,res->connectors[i]);
		if(!conn){
			fprintf(stderr,"cannot retrieve DRM connector %u:%u (%d) : %m\n",i,res->connectors[i],errno);
			continue;
		}

		device=malloc(sizeof(*device));
		if(memset(device,0,sizeof(*device))==NULL){
			fprintf(stderr,"Error in memset");
		}
		device->conn=conn->connector_id;

		ret=modes_setup_device(fd,res,conn,device);
		if(ret){
			if(ret!=-ENOENT){
				errno=-ret;
				fprintf(stderr,"cannot setup deviceice for connector %u:%u (%d): %m\n",i,res->connectors[i],errno);
			}
			free(device);
			drmModeFreeConnector(conn);
			continue;
		}

		drmModeFreeConnector(conn);
		device->next=modes_list;
		modes_list=device;
	}

	drmModeFreeResources(res);
	return 0;
}

static int modes_setup_device(int fd,drmModeRes *res,drmModeConnector *conn,struct modes_device *device)
{
	int ret;

	if(conn->connection!=DRM_MODE_CONNECTED){
		fprintf(stderr,"igonoring unused connector %u\n",conn->connector_id);
		return -ENOENT;
	}

	if(conn->count_modes==0){
		fprintf(stderr,"no valid mode for connector %u\n",conn->connector_id);
		return -EFAULT;
	}

	memcpy(&device->mode,&conn->modes[0],sizeof(device->mode));
	device->bufs[0].width=conn->modes[0].hdisplay;
	device->bufs[0].height=conn->modes[0].vdisplay;
	device->bufs[1].width=conn->modes[0].hdisplay;
	device->bufs[1].height=conn->modes[0].vdisplay;
	fprintf(stderr,"mode for connector is %u is %ux%u\n",conn->connector_id,device->bufs[0].width,device->bufs[0].height);

	ret=modes_find_crtc(fd,res,conn,device);
	if(ret){
		fprintf(stderr,"no valid crtc for connector %u\n",conn->connector_id);
		return ret;
	}

	ret=modes_create_fb(fd,&device->bufs[0]);
	

	if(ret){
		fprintf(stderr,"cannot create frame buffer for connector %u\n",conn->connector_id);
		return ret;
	}
	ret=modes_create_fb(fd,&device->bufs[1]);
	

	if(ret){
		fprintf(stderr,"cannot create frame buffer for connector %u\n",conn->connector_id);
		modes_destroy_fb(fd,&device->bufs[0]);
		return ret;
	}	

	return 0;
}

static int modes_find_crtc(int fd,drmModeRes *res,drmModeConnector *conn,struct modes_device *device)
{
	drmModeEncoder *enc;
	unsigned int i,j;
	int32_t crtc;
	struct modes_device *iter;

	if(conn->encoder_id){
		enc=drmModeGetEncoder(fd,conn->encoder_id);
	}
	else{
		enc=NULL;
	}

	if(enc){
		if(enc->crtc_id){
			crtc=enc->crtc_id;
			for(iter=modes_list;iter;iter=iter->next){
				if(iter->crtc==crtc){
					crtc=-1;
					break;
				}
			}

			if(crtc>=0){
				drmModeFreeEncoder(enc);
				device->crtc=crtc;
				return 0;
			}
		}
	
	drmModeFreeEncoder(enc);
	}

	for(i=0;i<conn->count_encoders;++i){
		enc=drmModeGetEncoder(fd,conn->encoders[i]);
		if(!enc){
			fprintf(stderr,"cannot retrieve encoder %u:%u (%d):%m \n",i,conn->encoders[i],errno);
			continue;
		}

		for(j=0;j<res->count_crtcs;++j){
			if(!(enc->possible_crtcs&(1<<j)))
				continue;

			crtc=res->crtcs[j];
			for(iter=modes_list;iter;iter=iter->next){
				if(iter->crtc==crtc){
					crtc=-1;
					break;
				}
			}

			if(crtc>=0){
				drmModeFreeEncoder(enc);
				device->crtc=crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr,"cannot find suitable CRTC for connector %u\n",conn->connector_id);
	return -ENOENT;
}

static int modes_create_fb(int fd,struct modes_buf *device){
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	memset(&creq,0,sizeof(creq));
	creq.width=device->width;
	creq.height=device->height;
	creq.bpp=32;
	ret=drmIoctl(fd,DRM_IOCTL_MODE_CREATE_DUMB,&creq);
	if(ret<0){
		fprintf(stderr,"cannot create dumb buffer (%d) : %m\n",errno);
		return -errno;
	}
	device->stride=creq.pitch;
	device->size=creq.size;
	device->handle=creq.handle;

	ret=drmModeAddFB(fd,device->width,device->height,24,32,device->stride,device->handle,&device->fb);
	if(ret){
		fprintf(stderr,"cannot create framebuffer (%d):%m\n",errno);
		ret=-errno;
		goto err_destroy;
	}

	memset(&mreq,0,sizeof(mreq));
	mreq.handle=device->handle;
	ret=drmIoctl(fd,DRM_IOCTL_MODE_MAP_DUMB,&mreq);
	if(ret){
		fprintf(stderr,"cannot map dumb buffer (%d): %m\n",errno);
		ret=-errno;
		goto err_fb;
	}
	device->map = mmap(0, device->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (device->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}
	if(memset(device->map, 0, device->size)==NULL){
		fprintf(stderr,"err in memset");
	}

	return 0;

err_fb:
	drmModeRmFB(fd, device->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = device->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

static void modes_destroy_fb(int fd,struct modes_buf *buf){
	struct drm_mode_destroy_dumb dreq;

	munmap(buf->map,buf->size);
	
	drmModeRmFB(fd,buf->size);

	memset(&dreq,0,sizeof(dreq));
	dreq.handle=buf->handle;
	drmIoctl(fd,DRM_IOCTL_MODE_DESTROY_DUMB,&dreq);
}

int main(int argc, char **argv)
{
	int ret, fd;
	const char *card;
	struct modes_device *iter;
	struct modes_buf *buf;

	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	fprintf(stderr, "using card '%s'\n", card);

	ret = modes_open(&fd, card);
	if (ret)
		goto out_return;

	ret = modes_prepare(fd);
	if (ret)
		goto out_close;
	for (iter = modes_list; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc);
		buf=&iter->bufs[iter->front_buf];
		ret = drmModeSetCrtc(fd, iter->crtc, buf->fb, 0, 0,
				     &iter->conn, 1, &iter->mode);
		if (ret)
			fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
				iter->conn, errno);
	}
	
	tcgetattr(STDIN_FILENO,&old_tio);
	new_tio = old_tio;
	new_tio.c_lflag &= (~ICANON & ~ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

	modes_draw(fd);

	modes_cleanup(fd);

	ret = 0;

out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modes failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return ret;
}

static void modes_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modes_device *device = data;

	device->pflip_pending = false;
	if (!device->cleanup)
	{	
		modes_draw_device(fd, device);
	}
}

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

static void check_input(void){
	char ch;
		ch=getchar();
		if(ch=='w'){
			dir=UP;

		}
		else if(ch=='s'){
			dir=DOWN;
		}
		else if(ch=='d'){
			printf("d has been pressed\n");
			dir=RIGHT;
		}
		else if(ch=='a'){
			dir=LEFT;
		}
		else if(ch=='q'){
			game_over=true;
		}
}

static bool check_position(int a,int b,int *arr_x,int *arr_y){
		if(rand()%130000<=34)
			return true;
		return false;
/*
 
	int i;
	for(i=0;i<arr_size;i++){
		if(arr_x[i]==a&&arr_y[i]==b){
		return true;
		}
	}


return false;/ *
	if(a==x&&b==y)
		return true;
	return false;*/
}

static void increment_position(int *arr_x,int *arr_y){
	int i;

	for(i=0;i<arr_size;i++){
		if(arr_x[i]%2==0){
			
			
		arr_x[i]+=rand()%8;
		arr_y[i]+=rand()%8;
		}
		else{
			
		arr_x[i]-=rand()%8;
		arr_y[i]-=rand()%8;
		}
	}		
}

static void modes_draw(int fd)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;
	struct modes_device *iter;

	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = modes_page_flip_event;

	for(m=0;m<arr_size;m++){
		x[m]=(rand()%((1920-250)-250))+250;
		y[m]=(rand()%((1080-200)-200))+200;
	}
	for (iter = modes_list; iter; iter = iter->next) {
		iter->r = rand() % 0xff;
		iter->g = rand() % 0xff;
		iter->b = rand() % 0xff;
		iter->r_up = iter->g_up = iter->b_up = true;

		
		
		modes_draw_device(fd, iter);
	}

	while (time(&cur) < start + 30) {
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		v.tv_sec = start + 30 - cur;

		ret = select(max(STDIN_FILENO,fd) + 1, &fds, NULL, NULL, &v);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(STDIN_FILENO, &fds)) {
			char c = getchar();
				fprintf(stderr, "%c was pressed\n",c);
			if(c == 'q' || c == 27){
				fprintf(stderr, "exit due to user-input\n");
				break;
			}
			
		} else if (FD_ISSET(fd, &fds)) {
			drmHandleEvent(fd, &ev);
		}
	}
}


static void modes_draw_device(int fd, struct modes_device *device)
{
	struct modes_buf *buf;
	unsigned int j, k, off,i=0;
	int ret;

	device->r = next_color(&device->r_up, device->r, 20);
	device->g = next_color(&device->g_up, device->g, 10);
	device->b = next_color(&device->b_up, device->b, 5);

	buf = &device->bufs[device->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		i=0;
		for (k = 0; k < buf->width; ++k) {
					off = buf->stride * j + k * 4;
					if((k>=250&&j>=200)&&(k<buf->width-250&&j<buf->height-200)){
						if(check_position(k,j,x,y)){
						*(uint32_t*)&buf->map[off] =
						  (255<<16)  | (255<<8) | 255;
						
						}else{
						*(uint32_t*)&buf->map[off] =
						  (0<<16)  | (0<<8) | 0;
						}
						
						if(i<arr_size)
							i++;
						else
							i=0;
				}
				else{
					*(uint32_t*)&buf->map[off] =
						     (device->r << 16) | (device->g << 8) | device->b;
				    }
				}
		}
	

	ret = drmModePageFlip(fd, device->crtc, buf->fb,
			      DRM_MODE_PAGE_FLIP_EVENT, device);
	if (ret) {
		fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
			device->conn, errno);
	} else {
		device->front_buf ^= 1;
		device->pflip_pending = true;
		increment_position(x,y);
		fprintf(stderr,"page has been flipped\n");
	}
}

static void modes_cleanup(int fd)
{
	struct modes_device *iter;
	drmEventContext ev;
	int ret;

	tcsetattr(STDIN_FILENO,TCSANOW,&old_tio);
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = modes_page_flip_event;

	while (modes_list) {
		iter = modes_list;
		modes_list = iter->next;

		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		if (!iter->pflip_pending)
			drmModeSetCrtc(fd,
				       iter->saved_crtc->crtc_id,
				       iter->saved_crtc->buffer_id,
				       iter->saved_crtc->x,
				       iter->saved_crtc->y,
				       &iter->conn,
				       1,
				       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		modes_destroy_fb(fd, &iter->bufs[1]);
		modes_destroy_fb(fd, &iter->bufs[0]);

		free(iter);
	}

}
