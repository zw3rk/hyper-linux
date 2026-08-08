#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdint.h>

static void dump_ximage(XImage *img, const char *tag) {
  fprintf(stderr, "%s: %dx%d depth=%d bpp=%d bpl=%d byte_order=%d bitmap_bit_order=%d red_mask=%lx\n",
    tag, img->width, img->height, img->depth, img->bits_per_pixel, img->bytes_per_line,
    img->byte_order, img->bitmap_bit_order, img->red_mask);
  if (img->data) {
    unsigned char *p = (unsigned char *)img->data;
    fprintf(stderr, "  first16:");
    for (int i=0;i<16 && i<img->bytes_per_line*img->height;i++) fprintf(stderr," %02x", p[i]);
    fprintf(stderr, "\n");
  }
}

static int try_put_sz(Display *d, Window w, int W, int H, unsigned long pixel) {
  Visual *vis = DefaultVisual(d, DefaultScreen(d));
  int depth = DefaultDepth(d, DefaultScreen(d));
  XImage *img = XCreateImage(d, vis, depth, ZPixmap, 0, NULL, W, H, 32, 0);
  if (!img) { fprintf(stderr, "create fail\n"); return -1; }
  img->data = calloc(1, (size_t)img->bytes_per_line * H);
  for (int y=0;y<H;y++) for (int x=0;x<W;x++) XPutPixel(img, x, y, pixel);
  dump_ximage(img, "before PutImage");
  XPutImage(d, w, DefaultGC(d, DefaultScreen(d)), img, 0,0,0,0,W,H);
  XSync(d, False);
  fprintf(stderr, "PutImage %dx%d pixel=0x%lx done\n", W, H, pixel);
  XDestroyImage(img);
  return 0;
}

int main(int argc, char **argv) {
  const char *mode = argc>1?argv[1]:"put2";
  Display *d = XOpenDisplay(NULL);
  if (!d) { fprintf(stderr,"open fail\n"); return 1; }
  int s = DefaultScreen(d);
  int W=120, H=90;
  Window win = XCreateSimpleWindow(d, RootWindow(d,s), 50,50,W,H,0, BlackPixel(d,s), BlackPixel(d,s));
  XStoreName(d, win, "hl-xprobe");
  XMapWindow(d, win); XSync(d, False);
  usleep(300000);
  if (!strcmp(mode,"fill")) {
    GC gc=XCreateGC(d,win,0,NULL);
    XColor c; Colormap cm=DefaultColormap(d,s);
    c.red=65535;c.green=0;c.blue=0;c.flags=DoRed|DoGreen|DoBlue;
    XAllocColor(d,cm,&c); XSetForeground(d,gc,c.pixel);
    XFillRectangle(d,win,gc,0,0,W,H); XFlush(d);
    fprintf(stderr,"fill pixel=0x%lx\n", (unsigned long)c.pixel);
  } else if (!strcmp(mode,"put2")) {
    try_put_sz(d, win, 2, 2, 0x00FF0000UL); /* try red */
  } else if (!strcmp(mode,"put")) {
    try_put_sz(d, win, W, H, 0x0000FF00UL);
  } else if (!strcmp(mode,"put1")) {
    try_put_sz(d, win, 1, 1, 0x00FF0000UL);
  } else if (!strcmp(mode,"shm") || !strcmp(mode,"shmrmid")) {
    /*
     * "shm"     — RMID last (segment alive for the whole exchange).
     * "shmrmid" — RMID immediately after XShmAttach, exactly as GTK+ 1.2
     *             gdkimage.c does, then keep writing pixels and PutImage.
     *             SysV semantics: the segment must survive until the last
     *             detach, so the server still sees the pixels written after
     *             the RMID. This is the XMMS skin path.
     */
    int rmid_early = !strcmp(mode,"shmrmid");
    if (!XShmQueryExtension(d)) { fprintf(stderr,"no shm\n"); }
    else {
      XShmSegmentInfo si; memset(&si,0,sizeof si);
      XImage *img=XShmCreateImage(d,DefaultVisual(d,s),DefaultDepth(d,s),ZPixmap,NULL,&si,W,H);
      si.shmid=shmget(IPC_PRIVATE,(size_t)img->bytes_per_line*H,IPC_CREAT|0777);
      fprintf(stderr,"shmget=%d\n", si.shmid);
      if (si.shmid>=0) {
        si.shmaddr=img->data=shmat(si.shmid,0,0); si.readOnly=0;
        if (XShmAttach(d,&si)) {
          XSync(d,False);
          if (rmid_early) {
            shmctl(si.shmid,IPC_RMID,0);
            fprintf(stderr,"RMID early (GTK+ 1.2 pattern)\n");
          }
          for(int y=0;y<H;y++) for(int x=0;x<W;x++) XPutPixel(img,x,y,0x0000FF00);
          dump_ximage(img,"shm");
          XShmPutImage(d,win,DefaultGC(d,s),img,0,0,0,0,W,H,False);
          XSync(d,False);
          fprintf(stderr,"shm put ok\n");
          XShmDetach(d,&si);
        } else fprintf(stderr,"attach fail\n");
        shmdt(si.shmaddr);
        if (!rmid_early) shmctl(si.shmid,IPC_RMID,0);
      } else perror("shmget");
      XDestroyImage(img);
    }
  } else if (!strcmp(mode,"bgpix") || !strcmp(mode,"bgpixshm")) {
    /*
     * Replicate the XMMS/GTK+ 1.2 skin chain end to end:
     *   image -> Pixmap (XPutImage or XShmPutImage)
     *         -> XSetWindowBackgroundPixmap
     *         -> XClearWindow  (server paints the window from the pixmap)
     * "bgpixshm" additionally uses MIT-SHM with the GTK RMID-after-attach
     * lifecycle, which is exactly what gdk_pixmap_create_from_xpm_d does.
     */
    int use_shm = !strcmp(mode,"bgpixshm");
    Visual *vis = DefaultVisual(d,s);
    int depth = DefaultDepth(d,s);
    Pixmap pm = XCreatePixmap(d, win, W, H, depth);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XImage *img = NULL;
    XShmSegmentInfo si; memset(&si,0,sizeof si);
    int ok = 1;

    if (use_shm) {
      if (!XShmQueryExtension(d)) { fprintf(stderr,"no shm\n"); ok = 0; }
      else {
        img = XShmCreateImage(d,vis,depth,ZPixmap,NULL,&si,W,H);
        si.shmid = shmget(IPC_PRIVATE,(size_t)img->bytes_per_line*H,IPC_CREAT|0777);
        if (si.shmid < 0) { perror("shmget"); ok = 0; }
        else {
          si.shmaddr = img->data = shmat(si.shmid,0,0); si.readOnly = 0;
          if (!XShmAttach(d,&si)) { fprintf(stderr,"attach fail\n"); ok = 0; }
          else { XSync(d,False); shmctl(si.shmid,IPC_RMID,0); }
        }
      }
    } else {
      img = XCreateImage(d,vis,depth,ZPixmap,0,NULL,W,H,32,0);
      if (img) img->data = calloc(1,(size_t)img->bytes_per_line*H);
      if (!img || !img->data) ok = 0;
    }

    if (ok && img) {
      for (int y=0;y<H;y++) for (int x=0;x<W;x++) XPutPixel(img,x,y,0x0000FF00);
      dump_ximage(img, use_shm ? "bgpixshm" : "bgpix");
      if (use_shm) XShmPutImage(d,pm,gc,img,0,0,0,0,W,H,False);
      else         XPutImage(d,pm,gc,img,0,0,0,0,W,H);
      XSync(d,False);
      XSetWindowBackgroundPixmap(d,win,pm);
      XClearWindow(d,win);
      XSync(d,False);
      fprintf(stderr,"%s: pixmap -> background -> clear done\n", mode);
    }
    if (use_shm && si.shmaddr) { XShmDetach(d,&si); shmdt(si.shmaddr); }
  }
  sleep(3);
  XCloseDisplay(d);
  return 0;
}
