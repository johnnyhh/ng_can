defmodule Ng.Can do
  use GenServer
  @moduledoc """
  Documentation for NgCan.
  """
  defmodule State do
    @moduledoc false
    defstruct [
      port: nil,                   # C port process
    ]
  end

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, [], opts)
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

  def handle_call({:open, name}, {from_pid, _}, state) do
    response = call_port(state, :open, name)
    {:reply, response, state}
  end

  #frames is a list of %{id: can_identifier, data: can_payload}
  def handle_call({:write, frame}, {from_pid, _}, state) do
    response = call_port(state, :write, {frame.id, frame.data})
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
