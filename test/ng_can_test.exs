defmodule NgCanTest do
  use ExUnit.Case
  doctest Ng.Can
  @can_interface "vcan0"

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
    {:ok, pid} = Ng.Can.start_link()
    :ok == GenServer.call(pid, {:open, @can_interface})
    {:ok, %{can_port: pid}}
  end

  test "write", %{can_port: port} do
    assert :ok == GenServer.call(port, {:write, []})
  end

end
