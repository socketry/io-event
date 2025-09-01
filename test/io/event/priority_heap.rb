# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024, by Samuel Williams.

require "io/event/priority_heap"

describe IO::Event::PriorityHeap do
	let(:priority_heap) {subject.new}
	
	with "empty heap" do 
		it "should return nil when the first element is requested" do
			expect(priority_heap.peek).to be_nil
		end
		
		it "should return nil when the first element is extracted" do
			expect(priority_heap.pop).to be_nil
		end
		
		it "should report its size as zero" do
			expect(priority_heap.size).to be(:zero?)
		end
		
		it "should report as empty" do
			expect(priority_heap).to be(:empty?)
		end
	end
	
	it "returns the same element after inserting a single element" do
		priority_heap.push(1)
		expect(priority_heap.size).to be == 1
		expect(priority_heap.pop).to be == 1
		expect(priority_heap.size).to be(:zero?)
	end
	
	with "#empty?" do
		it "should return false when heap contains elements" do
			priority_heap.push(1)
			expect(priority_heap).not.to be(:empty?)
		end
		
		it "should return true after popping all elements" do
			priority_heap.push(1)
			priority_heap.push(2)
			expect(priority_heap).not.to be(:empty?)
			
			priority_heap.pop
			expect(priority_heap).not.to be(:empty?)
			
			priority_heap.pop
			expect(priority_heap).to be(:empty?)
		end
	end
	
	it "should return inserted elements in ascending order no matter the insertion order" do
		(1..10).to_a.shuffle.each do |e|
			priority_heap.push(e)
		end
		
		expect(priority_heap.size).to be == 10
		expect(priority_heap.peek).to be == 1
		
		result = []
		10.times do
			result << priority_heap.pop
		end
		
		expect(result.size).to be == 10
		expect(priority_heap.size).to be(:zero?)
		expect(result.sort).to be == result
	end
	
	with "maintaining the heap invariant" do
		it "for empty heaps" do
			expect(priority_heap).to be(:valid?)
		end
		
		it "for heap of size 1" do
			priority_heap.push(123)
			expect(priority_heap).to be(:valid?)
		end
		
		it "for all permutations of size 5" do
			[1,2,3,4,5].permutation do |permutation|
				priority_heap.clear!
				permutation.each {|element| priority_heap.push(element)}
				expect(priority_heap).to be(:valid?)
			end
		end
		
		# A few examples with more elements (but not ALL permutations)
		it "for larger amounts of values" do
			5.times do
				priority_heap.clear!
				(1..1000).to_a.shuffle.each {|element| priority_heap.push(element)}
				expect(priority_heap).to be(:valid?)
			end
		end
		
		# What if we insert several of the same item along with others?
		it "with several elements of the same value" do
			test_values = (1..10).to_a + [4] * 5
			test_values.each {|element| priority_heap.push(element)}
			expect(priority_heap).to be(:valid?)
		end
	end
end
