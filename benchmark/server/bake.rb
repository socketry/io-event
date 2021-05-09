
SERVERS = ["compiled", "event.rb", "async.rb", "fork.rb", "thread.rb"]

def default
	build
	benchmark
end

def build
	compiler = ENV.fetch('CC', 'clang')
	system(compiler, "compiled.c", "-o", "compiled", chdir: __dir__)
end

def benchmark
	port = 9095
	wrk = ENV.fetch('WRK', 'wrk')
	
	SERVERS.each do |server|
		$stdout.puts "Benchmark #{server}..."
		
		pid = Process.spawn(File.expand_path(server, __dir__), port.to_s)
		puts "Server running pid=#{pid}..."
		
		sleep 1
		
		system(wrk, "-d10", "-t4", "-c8", "http://localhost:#{port}")
		
		Process.kill(:TERM, pid)
		_, status = Process.wait2(pid)
		puts "Server exited status=#{status}..."
		
		port += 1
	end
end
