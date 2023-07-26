# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require_relative 'event/version'
require_relative 'event/selector'

begin
	require 'IO_Event'
rescue LoadError => error
	warn "Could not load native event selector: #{error}"
	require_relative 'event/selector/nonblock'
end
