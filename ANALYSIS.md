# Analysis of HAProxy Handoff

A seamless reload starts by sending the master process a SIGUSR2. The strace
of the master process gets interesting at the point in which I see:

```
connect(13, {sa_family=AF_LOCAL, sun_path="/run/haproxy/admin.sock"}, 110) = 0
```

After the call to `connect(), the master issues:

```
sendto(13, "_getsocks\n", 10, 0, NULL, 0) = 10
```

This is a message sent along the admin socket to request file descriptors (of
the listening sockets). This in turn makes the child process invoke its
`\_getsocks` function defined in `src/cli.c` in the HAProxy codebase. The
`\_getsocks` function prepares the `SCM\_RIGHTS` message well-described in the
CloudFlare article [Know Your
SCM_RIGHTS](https://blog.cloudflare.com/know-your-scm_rights/).

The master process `recvmsg()`s and receives file descriptors `14`, `15`, `16,
`17` and `19`:

```
recvmsg(13, {msg_name(0)=NULL, msg_iov(1)=[{"\0\0\0\1\0\0\0\0\4\0\0\0\0\0\4\0\0\0\0\0\204\0\0\0\0\0\4\0\0\0", 1041854}], msg_controllen=40, {cmsg_len=36, cmsg_level=SOL_SOCKET, cmsg_type=SCM_RIGHTS, {14, 15, 16, 17, 19}}, msg_flags=0}, 0) = 30
```

These FDs correspond to the following sockets:

* `14` - Admin socket
* `15` - HTTPS listener
* `16` - HTTP listener
* `17` - Port 55555 listener
* `19` - Port 9000 listener

Next, the master sends a `SIGUSR1` to its child:

```
kill(12214, SIGUSR1)                    = 0
```

Then, it clones a new worker:

```
clone(child_stack=0, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x7f4134990c90) = 1157
```

And closes its open file descriptors including the ones previously sent via
`SCM_RIGHTS`:

```
close(14)                               = 0
close(15)                               = 0
close(16)                               = 0
close(17)                               = 0
close(19)                               = 0
```

You might be surprised by that. However, the new worker would have inherited its
parents FDs and the close (after clone) would not have affected the state of its
FDs. The handoff has effectively been completed. Gracefully.

You might be wondering, "what happens in/to the old worker after it receives the
SIGUSR1?"

# References

* [Beej's Guide to UNIX IPC](http://beej.us/guide/bgipc/html/multi/unixsock.html)
* [Blocking I/O, Nonblocking I/O, And Epoll](https://eklitzke.org/blocking-io-nonblocking-io-and-epoll)
* [HAProxy on GitHub](https://github.com/haproxy/haproxy)
* [Know Your SCM\_RIGHTS](https://blog.cloudflare.com/know-your-scm_rights/)

