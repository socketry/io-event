# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"

require "fiddle"

# Install a seccomp-BPF filter that makes pidfd_open(2) return EPERM.
# This simulates snap confinement (pre-snapd 2.75) where the seccomp profile
# blocks pidfd_open via its seccomp filter.
#
# MUST be called after fork — the filter applies to the calling process and all
# future children.
def install_pidfd_open_seccomp_block
	libc = Fiddle.dlopen(nil)
	prctl = Fiddle::Function.new(
		libc["prctl"],
		[Fiddle::TYPE_INT, Fiddle::TYPE_LONG, Fiddle::TYPE_LONG, Fiddle::TYPE_LONG, Fiddle::TYPE_LONG],
		Fiddle::TYPE_INT
	)

	# PR_SET_NO_NEW_PRIVS is required before installing a seccomp filter.
	raise "prctl(PR_SET_NO_NEW_PRIVS) failed" unless prctl.call(38, 1, 0, 0, 0) == 0

	# BPF program (4 instructions):
	#   ld  [0]            — load syscall number from seccomp_data
	#   jeq #434, 0, 1     — if pidfd_open, fall through; else skip to ALLOW
	#   ret #ERRNO|EPERM   — return EPERM
	#   ret #ALLOW         — allow the syscall
	filter = [
		[0x20, 0, 0, 0],
		[0x15, 0, 1, 434],
		[0x06, 0, 0, 0x00050001],
		[0x06, 0, 0, 0x7fff0000],
	].map { |code, jt, jf, k| [code, jt, jf, k].pack("vCCV") }.join

	filter_ptr = Fiddle::Pointer.malloc(filter.bytesize)
	filter_ptr[0, filter.bytesize] = filter

	# struct sock_fprog { unsigned short len; struct sock_filter *filter; }
	padding = Fiddle::SIZEOF_VOIDP - 2
	pack_ptr = Fiddle::SIZEOF_VOIDP == 8 ? "Q" : "L"
	prog = [4].pack("S") + ("\0" * padding) + [filter_ptr.to_i].pack(pack_ptr)

	prog_ptr = Fiddle::Pointer.malloc(prog.bytesize)
	prog_ptr[0, prog.bytesize] = prog

	# PR_SET_SECCOMP = 22, SECCOMP_MODE_FILTER = 2
	raise "prctl(PR_SET_SECCOMP) failed" unless prctl.call(22, 2, prog_ptr.to_i, 0, 0) == 0
end

ProcessWaitSignalfd = Sus::Shared("process wait signalfd fallback") do
	it "can wait for a process that has already exited" do
		child = fork do
			begin
				install_pidfd_open_seccomp_block
			rescue => error
				$stderr.puts "Seccomp not available: #{error}"
				exit!(2)
			end

			loop_fiber = Fiber.current
			sel = subject.new(loop_fiber)
			result = nil

			fiber = Fiber.new do
				pid = Process.spawn("true")
				result = sel.process_wait(Fiber.current, pid, 0)
			end

			fiber.transfer

			while fiber.alive?
				sel.select(1)
			end

			sel.close
			exit!(result&.success? ? 0 : 1)
		end

		_, status = Process.wait2(child)
		skip_unless_seccomp_available(status)
		expect(status.success?).to be == true
	end

	it "can wait for a process that is still running" do
		child = fork do
			begin
				install_pidfd_open_seccomp_block
			rescue => error
				$stderr.puts "Seccomp not available: #{error}"
				exit!(2)
			end

			loop_fiber = Fiber.current
			sel = subject.new(loop_fiber)
			result = nil

			fiber = Fiber.new do
				pid = Process.spawn("sleep 0.01")
				result = sel.process_wait(Fiber.current, pid, 0)
			end

			fiber.transfer

			while fiber.alive?
				sel.select(1)
			end

			sel.close
			exit!(result&.success? ? 0 : 1)
		end

		_, status = Process.wait2(child)
		skip_unless_seccomp_available(status)
		expect(status.success?).to be == true
	end

	it "can wait for two processes sequentially" do
		child = fork do
			begin
				install_pidfd_open_seccomp_block
			rescue => error
				$stderr.puts "Seccomp not available: #{error}"
				exit!(2)
			end

			loop_fiber = Fiber.current
			sel = subject.new(loop_fiber)
			result1 = result2 = nil

			fiber = Fiber.new do
				pid1 = Process.spawn("sleep 0")
				pid2 = Process.spawn("sleep 0")

				result1 = sel.process_wait(Fiber.current, pid1, 0)
				result2 = sel.process_wait(Fiber.current, pid2, 0)
			end

			fiber.transfer

			while fiber.alive?
				sel.select(1)
			end

			sel.close
			exit!(result1&.success? && result2&.success? ? 0 : 1)
		end

		_, status = Process.wait2(child)
		skip_unless_seccomp_available(status)
		expect(status.success?).to be == true
	end

	def skip_unless_seccomp_available(status)
		skip "seccomp filter not available" if status.exitstatus == 2
	end
end

IO::Event::Selector.constants.each do |name|
	next unless name == :EPoll || name == :URing

	klass = IO::Event::Selector.const_get(name)

	describe(klass, unique: "#{name}_signalfd") do
		it_behaves_like ProcessWaitSignalfd
	end
end
