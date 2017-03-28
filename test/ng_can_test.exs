defmodule NgCanTest do
  use ExUnit.Case
  doctest Ng.Can
  require Logger
  #we can use the same interface if
  #CAN_RAW_LOOPBACK = 1 and CAN_RAW_RECV_OWN_MSGS = 0
  @can1_interface "vcan0"
  @can2_interface "vcan0"

  defp setup_vcan do
    # Not sure how sudo works in elixir context,
    # so I've been running manually on start:

    # modprobe can
    # modprobe can_raw
    # modprobe vcan
    # sudo ip link add dev vcan0 type vcan
    # sudo ip link set up vcan0
    # ip link show vcan0
  end

  setup do
    :os.cmd('rm *.log')
    {:ok, can1} = Ng.Can.start_link()
    Logger.info "can1 is: #{inspect can1}"
    {:ok, can2} = Ng.Can.start_link()
    Logger.info "can2 is: #{inspect can2}"
    {:ok, %{can1: can1, can2: can2}}
  end

  #  test "basic write + read", %{can1: can1, can2: can2} do
  #    :ok = Ng.Can.open(can2, @can2_interface)
  #    :ok = Ng.Can.open(can1, @can1_interface, sndbuf: 1024)
  #    <<id1::size(32)>> = <<1, 2, 3, 4>>
  #    <<id2::size(32)>> = <<4, 3, 2, 1>>
  #    frame1 = {id1, <<97,97,97,97,97,97,97,97>>}
  #    #pad short frames
  #    frame2 = {id2, <<98,98,98,98,98,98,98>>}
  #    :ok = Ng.Can.write(can1, [frame1, frame2])
  #    :ok = Ng.Can.await_read(can2)
  #    receive do
  #      {:can_frames, _foobar, [rf1, rf2]} ->
  #        assert rf1 == frame1
  #        assert rf2 == {id2, <<98,98,98,98,98,98,98,0>>}
  #    after
  #      1000 -> raise "await data timed out"
  #    end
  #  end

  test "write + read - fill writebuf", %{can1: can1, can2: can2} do
    :ok = Ng.Can.open(can1, @can1_interface, sndbuf: 1024)
    :ok = Ng.Can.open(can2, @can2_interface, rcvbuf: 106496)
    frames = Enum.reduce (1..100), [], fn i, frames ->
      <<id::size(32)>> = <<1,2,3,i>>
      [{id, <<1,2,3,4,5,6,7,i>>} | frames]
    end
    :ok = Ng.Can.write(can1, frames)
    recv_frames(can2, frames)
    assert true
  end

  defp recv_frames(reader, sent_frames, recvd_frames \\ []) do
    :ok = Ng.Can.await_read(reader)
    receive do
      {:can_frames, _, new_frames} ->
        recvd_frames = recvd_frames ++ new_frames
        if length(recvd_frames) == length(sent_frames) do
          assert recvd_frames == sent_frames
        else
          IO.puts "only got #{length(recvd_frames)}"
          recv_frames(reader, sent_frames, recvd_frames)
        end
      other ->
        raise "wrong msg recvd"
    after 
      3000 ->
        raise "timed out waiting for frames"
    end
  end

end
