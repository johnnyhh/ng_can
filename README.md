# NgCan
a simple elixir library to read from and write to a can/canfd bus. since it uses socketCAN, it only runs on Linux.

## Usage

**opening a can port**
```
{:ok, can_port} = Ng.Can.start_link
Ng.Can.open(can_port, "vcan0", "canfd", sndbuf: 1024, rcvbuf: 106496)
```
To run can replace "canfd" with "can".

**writing to a can port**
```
<<id::size(32)>> = <<1,2,3,4>>
frame = {id, <<1,2,3,4,5,6,7,8>>}
Ng.Can.write(can_port, frame)
#write can also take an array of frames
```

**reading from a can port**

`Ng.Can.await_read/1` works the same way as the `:once` option in erlang's `:gen_udp.open/2`. If there's any data in the can port's receive buffer, `await_read` will immediately message the calling process with the buffered frames. Otherwise, it will message the calling process with new frames once they come in.
```
Ng.Can.await_read(can_port)
receive do
  {:can_frames, _interface_name, recvd_frames} ->
    Logger.info "got an array of frames: #{inspect recvd_frames}"
  other ->
    raise "wrong msg recvd"
```
