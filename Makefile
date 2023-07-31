OBJS := detect.o
LIBS := -lvaal
CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-plugins-base-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0)
LIBS += $(shell pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-plugins-base-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0)

%.o : %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS) 

detect: $(OBJS)
	dpkg -L libvaal
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)


install: detect
	mkdir -p $(WORKDIR)
	cp detect $(WORKDIR)/


clean:
	rm -f *.o
	rm -f detect
