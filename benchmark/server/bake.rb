# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

SERVERS = [
	"compiled",
	"event.rb",
	"buffer.rb",
	"loop.rb",
	"async.rb",
	"thread.rb",
	"fork.rb",
]

def default
	build
	benchmark
end

def build
	compiler = ENV.fetch('CC', 'clang')
	system(compiler, "compiled.c", "-o", "compiled", chdir: __dir__)
end

# @parameter connections [Integer] The number of simultaneous connections.
# @parameter threads [Integer] The number of client threads to use.
# @parameter duration [Integer] The duration of the test.
def benchmark(connections: 8, threads: 1, duration: 1)
	port = 9095
	wrk = ENV.fetch('WRK', 'wrk')
	
	SERVERS.each do |server|
		$stdout.puts [nil, "Benchmark #{server}..."]
		
		pid = Process.spawn(File.expand_path(server, __dir__), port.to_s)
		puts "Server running pid=#{pid}..."
		
		sleep 1
		
		system(wrk, "-d#{duration}", "-t#{threads}", "-c#{connections}", "http://localhost:#{port}")
		
		Process.kill(:TERM, pid)
		_, status = Process.wait2(pid)
		puts "Server exited status=#{status}..."
		
		port += 1
	end
end
