defmodule Ng.Can do
  use GenServer
  @moduledoc """
  Documentation for NgCan.
  """
  defmodule State do
    @moduledoc false
    defstruct [
      port: nil,                   # C port process
      controlling_process: nil,
      interface: nil
    ]
  end

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, [], opts)
  end

  def write(pid, frames) when is_list(frames) do
    GenServer.call(pid, {:write, frames})
  end
  def write(pid, frames) do
    write(pid, [frames])
  end

  def open(pid, name) do
    GenServer.call(pid, {:open, name})
  end

  def await_read(pid) do
    GenServer.call(pid, :await_read)
  end

  def init(_args) do
    executable = :code.priv_dir(:ng_can) ++ '/ng_can'
    port = Port.open({:spawn_executable, executable},
      [{:args, []},
        {:packet, 2},
        :use_stdio,
        :binary,
        :exit_status])
    state = %State{port: port}
    {:ok, state}
  end

  #notification stuff
  def handle_info({_, {:data, <<?n, message::binary>>}}, state) do
    {:notif, frames} = :erlang.binary_to_term(message)
    frames
    |> Enum.map(fn {id, data} -> %{id: id, data: data} end)
    |> send_frames(state)
  end

  defp send_frames(frames, state) do
    if state.controlling_process do
      send(state.controlling_process, {:can_frames, state.interface, frames})
    end
    {:noreply, state}
  end

  def handle_call({:open, interface}, {from_pid, _}, state) do
    response = call_port(state, :open, interface)
    {:reply, response, %{state | controlling_process: from_pid, interface: interface}}
  end

  #frames is a list of %{id: can_identifier, data: can_payload}
  def handle_call({:write, frames}, {from_pid, _}, state) do
    formatted_frames = Enum.map frames, fn frame ->
      {frame.id, frame.data}
    end
    response = call_port(state, :write, formatted_frames)
    {:reply, response, state}
  end

  def handle_call(:read, {from_pid, _}, state) do
    response = call_port(state, :read, nil)
    {:reply, response, state}
  end

  def handle_call(:await_read, {from_pid, _}, state) do
    response = call_port(state, :await_read, nil)
    {:reply, response, state}
  end

  def terminate(reason, state) do
    IO.puts "Going to terminate: #{inspect reason}"
    Port.close(state.port)
  end

  defp call_port(state, command, arguments, timeout \\ 4000) do
    msg = {command, arguments}
    send state.port, {self(), {:command, :erlang.term_to_binary(msg)}}
    # Block until the response comes back since the C side
    # doesn't want to handle any queuing of requests. REVISIT
    receive do
      {_, {:data, <<?r,response::binary>>}} ->
        :erlang.binary_to_term(response)
    after
      timeout ->
        # Not sure how this can be recovered
        exit(:port_timed_out)
    end
  end

end
