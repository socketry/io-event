# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require_relative 'environment'

Warning[:experimental] = false

require "covered/sus"
include Covered::Sus

# Intensive GC checking:
#
# Thread.new do
# 	while true
# 		sleep 0.0001
# 		$stderr.puts GC.verify_compaction_references
# 	end
# end
