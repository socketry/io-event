unless Object.const_defined?(:UNIXSocket)
	class UNIXSocket
		def self.pair(&block)
			Socket.pair(:INET, :STREAM, 0, &block)
		end
	end
end
