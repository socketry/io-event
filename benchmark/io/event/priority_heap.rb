# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "sus/fixtures/benchmark"
require "io/event/priority_heap"

describe IO::Event::PriorityHeap do
	include Sus::Fixtures::Benchmark
	
	let(:entries) {(0...10_000).to_a}
	let(:append_entries) {(10_000...15_000).to_a}
	
	def build_heap(entries)
		heap = subject.new
		heap.concat(entries)
		return heap
	end
	
	measure "push in-order entries" do |repeats|
		heap = subject.new
		index = 0
		
		repeats.times do
			heap.push(index)
			index += 1
		end
	end
	
	measure "push reverse-order entries" do |repeats|
		heap = subject.new
		index = 0
		
		repeats.times do
			heap.push(-index)
			index += 1
		end
	end
	
	measure "concat entries into empty heap" do |repeats|
		repeats.times do
			heap = subject.new
			heap.concat(entries)
		end
	end
	
	measure "pop entries" do |repeats|
		heap = build_heap(entries)
		
		repeats.times do
			if heap.empty?
				heap.concat(entries)
			end
			
			heap.pop
		end
	end
	
	measure "heapify after deleting half and appending half" do |repeats|
		repeats.times do
			heap = build_heap(entries)
			
			heap.heapify do |contents|
				contents.delete_if{|element| element.even?}
				contents.concat(append_entries)
			end
		end
	end
	
	measure "delete_if half the entries" do |repeats|
		repeats.times do
			heap = build_heap(entries)
			
			heap.delete_if do |element|
				element.even?
			end
		end
	end
end
