defmodule Ng.Can do
  use GenServer
  require Logger
  @moduledoc """
  Documentation for NgCan.
  """
  @default_bufsize 106496
  defmodule State do
    defstruct [
      port: nil,
      awaiting_process: nil,
      interface: nil
    ]
  end

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, [], opts)
  end

  def start(opts \\ []) do
    GenServer.start(__MODULE__, [], opts)
  end

  def write(pid, frames) when is_list(frames) do
    GenServer.call(pid, {:write, pad_to_8_bytes(frames)})
  end
  def write(pid, frames) do
    write(pid, [frames])
  end

  def open(pid, name, args \\[]) do
    GenServer.call(pid, {:open, name, args})
  end

  def await_read(pid) do
    GenServer.call(pid, :await_read)
  end

  def init(args) do
    executable = :code.priv_dir(:ng_can) ++ '/ng_can'
    port = Port.open({:spawn_executable, executable},
      [{:args, []},
        {:packet, 2},
        :use_stdio,
        :binary,
        :exit_status])
    {:ok, %State{port: port}}
  end

  #port communication
  def handle_info({_, {:data, <<?n, message::binary>>}}, state) do
    {:notif, frames} = :erlang.binary_to_term(message)
    send_frames(frames, state)
  end

  def handle_info({_, {:exit_status, status}}, state) do
    Logger.info("can port exited with status: #{inspect status}")
    exit(:port_err)
  end

  defp send_frames(frames, state) do
    if state.awaiting_process do
      send(state.awaiting_process, {:can_frames, state.interface, frames})
    end
    {:noreply, state}
  end

  def handle_call({:open, interface, args}, {from_pid, _}, state) do
    response = call_port(state, :open,
                         {interface, args[:rcvbuf] || @default_bufsize,
                           args[:sndbuf] || @default_bufsize
                         })
    {:reply, response, %{state | interface: interface}}
  end

  #frames is a list of tuples {can_identifier, can_payload}
  def handle_call({:write, frames}, {from_pid, _}, state) do
    response = call_port(state, :write, frames)
    {:reply, response, state}
  end

  def handle_call(:read, {from_pid, _}, state) do
    response = call_port(state, :read, nil)
    {:reply, response, state}
  end

  def handle_call(:await_read, {from_pid, _}, state) do
    response = call_port(state, :await_read, nil)
    {:reply, response, %{state | awaiting_process: from_pid}}
  end

  def terminate(reason, state) do
    Logger.info "Ng.Can terminating with reason: #{inspect reason}"
  end

  defp pad_to_8_bytes(frames) do
    Enum.map frames, fn {id, data} ->
      bits_padding = (8 - byte_size(data)) * 8
      padded_data = data <> <<0 :: size(bits_padding)>>
      {id, padded_data}
    end
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
        Port.close(state.port)
        exit(:port_timed_out)
    end
  end

end
