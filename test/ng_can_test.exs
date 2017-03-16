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
    {:ok, can1} = Ng.Can.start_link()
    :ok == GenServer.call(can1, {:open, @can1_interface})
    {:ok, can2} = Ng.Can.start_link()
    :ok == GenServer.call(can2, {:open, @can2_interface})
    {:ok, %{can1: can1, can2: can2}}
  end

  test "write + read", %{can1: can1, can2: can2} do
    foodata = <<97,97,97,97,97,97,97,97>>
    fooid = <<1, 2, 3, 4>>
    frame = %{id: fooid, data: foodata}
    :ok = GenServer.call(can1, {:write, frame})
    assert {:error, :nodata} == GenServer.call(can1, :read)
    {:ok, {id, recvd_data}} = GenServer.call(can2, :read)
    assert recvd_data == foodata
  end

end
