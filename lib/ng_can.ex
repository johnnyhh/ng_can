defmodule Ng.Can do
  use GenServer
  require Logger
  @moduledoc """
  Documentation for NgCan.
  """
  @default_bufsize 106496
  #keep up to 1000 can frames in state, serve up to 100 at a time
  @rcv_bufsize 1000
  @rcv_chunksize 100
  defmodule State do
    defstruct [
      port: nil,
      awaiting_process: nil,
      awaiting_read: false,
      interface: nil,
      #new frames are added to the front of the list
      rcvbuf: [],
      rcvbuf_len: 0
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
    GenServer.cast(pid, :await_read)
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
    {:notif, frames, num_frames} = :erlang.binary_to_term(message)
    state = enqueue_frames(num_frames, frames, state)
    {:noreply, forward_frames(state)}
  end

  #port error
  def handle_info({_, {:data, <<?e, message::binary>>}}, state) do
    log_error message
    {:noreply, state}
  end

  def handle_info({_, {:data, <<?r, _::binary>>}}, state), do: {:noreply, state}

  def handle_info({_, {:exit_status, status}}, state) do
    Logger.info("can port exited with status: #{inspect status}")
    exit(:port_err)
  end

  defp enqueue_frames(num_frames, frames, state) do
    new_buffer = state.rcvbuf ++ frames
    num_to_trash = num_frames + state.rcvbuf_len - 1000
    if num_to_trash > 0 do
      {_trashed, new_buffer} = Enum.split(new_buffer, num_to_trash)
      %{state | rcvbuf: new_buffer, rcvbuf_len: 1000}
    else
      %{state | rcvbuf: new_buffer,
        rcvbuf_len: num_frames + state.rcvbuf_len}
    end
  end

  defp forward_frames(%{awaiting_process: nil} = state), do: state
  defp forward_frames(%{awaiting_read: false} = state), do: state
  defp forward_frames(%{rcvbuf_len: 0} = state), do: state
  defp forward_frames(state) do
    num_remaining = max(0, state.rcvbuf_len - 100)
    {to_send, unsent} = Enum.split(state.rcvbuf, 100)
    send(state.awaiting_process, {:can_frames, state.interface, to_send})
    %{state | rcvbuf: unsent, rcvbuf_len: num_remaining, awaiting_read: false}
  end

  def handle_call({:open, interface, args}, {from_pid, _}, state) do
    :os.cmd('ip link set #{interface} type can bitrate 250000 triple-sampling on restart-ms 100')
    :os.cmd 'ip link set #{interface} up type can'
    :os.cmd 'ifconfig #{interface} txqueuelen 1000'
    response = call_port(state, :open,
                         {interface, args[:rcvbuf] || @default_bufsize,
                           args[:sndbuf] || @default_bufsize
                         })
    {:reply, response, %{state | awaiting_process: from_pid, interface: interface}}
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

  def handle_cast(:await_read, state) do
    {:noreply, forward_frames(%{state | awaiting_read: true})}
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
      {_, {:data, <<?e, message::binary>>}} ->
        log_error message
        :port_err
    after
      timeout ->
        # Not sure how this can be recovered
        Port.close(state.port)
        exit(:port_timed_out)
    end
  end

  defp log_error(error_response) do
    {:error, msg} = :erlang.binary_to_term(error_response)
    Logger.error("Ng.Can C port reported error: #{msg}")
  end

end
