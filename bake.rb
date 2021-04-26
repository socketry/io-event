
def build
	ext_path = File.expand_path("ext/event", __dir__)
	
	Dir.chdir(ext_path) do
		system("./extconf.rb")
		system("make")
	end
end
