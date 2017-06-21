#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>

#include <cassert>
#include <thread>

#include "net_utils.hpp"
#include "ring_buffer.hpp"

#define MAXEVENTS 64

#define DEFAULT_BUFFER_SIZE 4096
#define DEFAULT_RING_BUFFER_SIZE 1024

// Event data includes the socket FD,
// a pointer to a fixed sized buffer,
// number of bytes written and 
// a boolean indicating whether the worker thread should stop.

//事件的数据类型
struct event_data {
  int fd;             //描述符
  char* buffer;		  //字符缓存
  int written;        //写入的个数
  bool stop;          //客户端发送exit，即可停止时间  

  event_data():
  fd(-1),
  buffer(new char[DEFAULT_RING_BUFFER_SIZE]),
  written(0),
  stop(false) {

  }

  ~event_data() {
    delete[] buffer;
  }
};

// Reverse in place.

// 字符倒序处理
void reverse(char* p, int length) {
  int length_before_null = length - 1;
  int i,j;

  for (i = 0, j = length_before_null; i < j; i++, j--) {
    char temp = p[i];
    p[i] = p[j];
    p[j] = temp;
  }
}

int process_messages(processor::RingBuffer<event_data>* ring_buffer) {      //缓冲处理函数：processor为命名空间，RingBuffer为缓冲类，
																			//event_data定义在类的事件类型，由RingBuffer.hpp的模板T调用给events_
  int64_t prev_sequence = -1;
  int64_t next_sequence = -1;
  int num_events_processed = 0;
  int s = -1;
  while (true) {
    // Spin till a new sequence is available.
    while (next_sequence <= prev_sequence) {
      _mm_pause();
      next_sequence = ring_buffer->getProducerSequence();
    }
    // Once we have a new sequence process all items between the previous sequence and the new one.
    for (int64_t index = prev_sequence + 1; index <= next_sequence; index++) {
      auto e_data = ring_buffer->get(index);    //获取队列的当前事件                              
      auto client_fd = e_data->fd;              //反馈给client端的fd
      auto buffer = e_data->buffer;             //当前事件的缓冲，后面会赋值给client的fd
      // Used to signal the server to stop.
      //printf("\nConsumer stop value %s\n", e_data->stop ? "true" : "false");
      if (e_data->stop)
        goto exit_consumer;

      auto buffer_length = e_data->written;
      assert(client_fd != -1);
      assert(buffer_length > 0);

      // Write the buffer to standard output first.
      s = write (1, buffer, buffer_length);   //client发送的数据显示在服务端
      if (s == -1) {
        perror ("write");
        abort ();
      }

      // Then reverse it and echo it back.
      reverse(buffer, buffer_length);         //倒排server收到client的数据
      s = write(client_fd, buffer, buffer_length);   //将buffer回写到client端
      if (s == -1) {
        perror ("echo");
        abort ();
      }
      // We are not checking to see if all the bytes have been written.
      // In case they are not written we must use our own epoll loop, express write interest
      // and write when the client socket is ready.
      ++num_events_processed;
    }
    // Mark events consumed.
    ring_buffer->markConsumed(next_sequence);
    prev_sequence = next_sequence;
  }
exit_consumer:
  printf("Finished processing all events. Server shutting down. Num events processed = %d\n", num_events_processed);
  return 1;
}

void event_loop(int epfd,
                int sfd,
                processor::RingBuffer<event_data>* ring_buffer) {   //循环监听活跃的fd，并将数据写入ring_buffer,
																	//这里使用指针写入的，比较关键，和process_messages函数实现多线程数据共享
  int n, i;
  int retval;

  struct epoll_event event, current_event;
  // Buffer where events are returned.
  struct epoll_event* events = static_cast<epoll_event*>(calloc(MAXEVENTS, sizeof event));

  while (true) {

    n = epoll_wait(epfd, events, MAXEVENTS, -1);   //监听活跃的fd

    for (i = 0; i < n; i++) {            //循环处理当前fd
      current_event = events[i];

      if ((current_event.events & EPOLLERR) ||     //容错处理
          (current_event.events & EPOLLHUP) ||
          (!(current_event.events & EPOLLIN))) {
        // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?).
        fprintf(stderr, "epoll error\n");
        close(current_event.data.fd);
      } else if (current_event.events & EPOLLRDHUP) {    // 套接字挂断处理
        
         // Stream socket peer closed connection, or shut down writing half of connection.
        // We still to handle disconnection when read()/recv() return 0 or -1 just to be sure.
        printf("Closed connection on descriptor vis EPOLLRDHUP %d\n", current_event.data.fd);
        // Closing the descriptor will make epoll remove it from the set of descriptors which are monitored.
        close(current_event.data.fd);
      } else if (sfd == current_event.data.fd) {        //匹配活跃的fd，并处理
 
        // We have a notification on the listening socket, which means one or more incoming connections.
        while (true) {
          struct sockaddr in_addr;
          socklen_t in_len;
          int infd;
          char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

          in_len = sizeof in_addr;
          // No need to make these sockets non blocking since accept4() takes care of it.
          infd = accept4(sfd, &in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);    //获得活跃的fd之后，接收产出连接，
																				   //生成infd（利用accept4 设置infd为非阻塞）
          if (infd == -1) {
            if ((errno == EAGAIN) ||
                (errno == EWOULDBLOCK)) {
              break;  // We have processed all incoming connections.
            } else {
              perror("accept");
              break;
            }
          }

         // Print host and service info.
          retval = getnameinfo(&in_addr, in_len,                           //利用IP获取主机名和端口
                               hbuf, sizeof hbuf,
                               sbuf, sizeof sbuf,
                               NI_NUMERICHOST | NI_NUMERICSERV);
          if (retval == 0) {
            printf("Accepted connection on descriptor %d (host=%s, port=%s)\n", infd, hbuf, sbuf);
          }

         // Register the new FD to be monitored by epoll.
          event.data.fd = infd;
          // Register for read events, disconnection events and enable edge triggered behavior for the FD.
          event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
          retval = epoll_ctl(epfd, EPOLL_CTL_ADD, infd, &event);
          if (retval == -1) {
            perror("epoll_ctl");
            abort();
          }
        }
      } else {
        // We have data on the fd waiting to be read. Read and  display it.
        // We must read whatever data is available completely, as we are running in edge-triggered mode
        // and won't get a notification again for the same data.
        bool should_close = false, done = false;

        while (!done) {                                     //事件在非阻塞情况下，循环处理，直到fd没有数据
          ssize_t count;
          // Get the next ring buffer entry.
          auto next_write_index = ring_buffer->nextProducerSequence();
          auto entry = ring_buffer->get(next_write_index);

          // Read the socket data into the buffer associated with the ring buffer entry.
          // Set the entry's fd field to the current socket fd.
          count = read(current_event.data.fd, entry->buffer, DEFAULT_BUFFER_SIZE);     //读取事件数据，写入ring buffer
          entry->written = count;
          entry->fd = current_event.data.fd;

          if (count == -1) {
            // EAGAIN or EWOULDBLOCK means we have no more data that can be read.
            // Everything else is a real error.
            if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
              perror("read");
              should_close = true;
            }
            done = true;
          } else if (count == 0) {
            // Technically we don't need to handle this here, since we wait for EPOLLRDHUP. We handle it just to be sure.
            // End of file. The remote has closed the connection.
            should_close = true;
            done = true;
          } else {
            // Valid data. Process it.
            // Check if the client want's the server to exit.
            // This might never work out even if the client sends an exit signal because TCP might
            // split and rearrange the packets across epoll signal boundaries at the server.
            bool stop = (strncmp(entry->buffer, "exit", 4) == 0);         //检测退出，比如在客户端发送“exit”字符，服务器会退出接收事件
            entry->stop = stop;

            // Publish the ring buffer entry since all is well.
            ring_buffer->publish(next_write_index);                    //将entry数据，回写到ring buffer
            if (stop)
              goto exit_loop;
          }
        }


        if (should_close) {
          printf("Closed connection on descriptor %d\n", current_event.data.fd);
          // Closing the descriptor will make epoll remove it from the set of descriptors which are monitored.
          close(current_event.data.fd);
        }
      }
    }
  }
exit_loop:
  free(events);
}

int main (int argc, char *argv[]) {
  int sfd, epfd, retval;
  // Our ring buffer.
  auto ring_buffer = new processor::RingBuffer<event_data>(DEFAULT_RING_BUFFER_SIZE);

  if (argc != 2) {
    fprintf(stderr, "Usage: %s [port]\n", argv[0]);
    exit (EXIT_FAILURE);
  }

  sfd = create_and_bind(argv[1]);   //创建和绑定套接字
  if (sfd == -1)
    abort ();

  retval = make_socket_non_blocking(sfd);   //设置套接字为非阻塞类型
  if (retval == -1)
    abort ();

  retval = listen(sfd, SOMAXCONN);  //监听套接字
  if (retval == -1) {
    perror ("listen");
    abort ();
  }

  epfd = epoll_create1(0);    //创建epoll文件描述集
  if (epfd == -1) {
    perror ("epoll_create");
    abort ();
  }

  // Register the listening socket for epoll events.    //将fd注册到epoll
  {
    struct epoll_event event;
    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    retval = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &event);
    if (retval == -1) {
      perror ("epoll_ctl");
      abort ();
    }
  }


  // Start the worker thread.
  std::thread t{process_messages, ring_buffer};  //启动数据处理线程

  // Start the event loop.
  event_loop(epfd, sfd, ring_buffer);   	     //启动事件循环监听

  // Our server is ready to stop. Release all pending resources.
  t.join();                                     // 等待线程销毁
  close(sfd);
  delete ring_buffer;

  return EXIT_SUCCESS;
}
