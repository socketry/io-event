# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.

require_relative "environment"

Warning[:experimental] = false

if ENV.key?("COVERAGE")
	require "covered/sus"
	include Covered::Sus
end

# Intensive GC checking:
#
# Thread.new do
# 	while true
# 		sleep 0.0001
# 		$stderr.puts GC.verify_compaction_references
# 	end
# end
