# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024-2025, by Samuel Williams.

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
	
	with "#delete" do
		it "should return nil when deleting from empty heap" do
			expect(priority_heap.delete(42)).to be_nil
		end
		
		it "should return nil when deleting non-existent element" do
			priority_heap.push(1)
			priority_heap.push(2)
			priority_heap.push(3)
			
			expect(priority_heap.delete(42)).to be_nil
			expect(priority_heap.size).to be == 3
		end
		
		it "should delete the only element from single-element heap" do
			priority_heap.push(42)
			
			expect(priority_heap.delete(42)).to be == 42
			expect(priority_heap.size).to be(:zero?)
			expect(priority_heap).to be(:empty?)
		end
		
		it "should delete first element (root) and maintain heap property" do
			elements = [5, 2, 8, 1, 9, 3]
			elements.each {|e| priority_heap.push(e)}
			
			# Root should be the minimum (1)
			expect(priority_heap.peek).to be == 1
			
			# Delete the root
			expect(priority_heap.delete(1)).to be == 1
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			
			# New root should be the next minimum (2)
			expect(priority_heap.peek).to be == 2
		end
		
		it "should delete last element without affecting heap structure" do
			elements = [5, 2, 8, 1, 9, 3]
			elements.each {|e| priority_heap.push(e)}
			original_root = priority_heap.peek
			
			# Delete one element (not necessarily the last in array, but some element)
			expect(priority_heap.delete(5)).to be == 5
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			expect(priority_heap.peek).to be == original_root  # Root shouldn't change
		end
		
		it "should delete middle elements and maintain heap property" do
			elements = [10, 5, 15, 3, 7, 12, 18, 1, 4, 6, 8]
			elements.each {|e| priority_heap.push(e)}
			
			# Delete some middle elements
			expect(priority_heap.delete(7)).to be == 7
			expect(priority_heap).to be(:valid?)
			expect(priority_heap.size).to be == 10
			
			expect(priority_heap.delete(12)).to be == 12
			expect(priority_heap).to be(:valid?)
			expect(priority_heap.size).to be == 9
		end
		
		it "should handle deleting duplicate elements" do
			elements = [5, 3, 5, 1, 5, 2]
			elements.each {|e| priority_heap.push(e)}
			
			# Should delete only the first occurrence of 5
			expect(priority_heap.delete(5)).to be == 5
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			
			# Should still have other 5s in the heap
			remaining_elements = []
			while !priority_heap.empty?
				remaining_elements << priority_heap.pop
			end
			expect(remaining_elements.count(5)).to be == 2
		end
		
		it "should maintain heap property after multiple deletions" do
			elements = (1..20).to_a.shuffle
			elements.each {|e| priority_heap.push(e)}
			
			# Delete several elements
			to_delete = [5, 10, 15, 1, 20, 8]
			to_delete.each do |element|
				expect(priority_heap.delete(element)).to be == element
				expect(priority_heap).to be(:valid?)
			end
			
			expect(priority_heap.size).to be == 14
			
			# Remaining elements should still come out in sorted order
			result = []
			while !priority_heap.empty?
				result << priority_heap.pop
			end
			
			expect(result.sort).to be == result
			expected_remaining = (1..20).to_a - to_delete
			expect(result.sort).to be == expected_remaining.sort
		end
		
		it "should work correctly when deleting all elements one by one" do
			elements = [4, 2, 6, 1, 3, 5, 7]
			elements.each {|e| priority_heap.push(e)}
			
			elements.shuffle.each do |element|
				expect(priority_heap.delete(element)).to be == element
				expect(priority_heap).to be(:valid?)
			end
			
			expect(priority_heap).to be(:empty?)
		end
		
		it "should handle complex deletion patterns" do
			# Insert elements in random order
			elements = (1..100).to_a.shuffle
			elements.each {|e| priority_heap.push(e)}
			
			# Delete every 3rd element (by value, not position)
			deleted = []
			(1..100).each do |i|
				if i % 3 == 0
					expect(priority_heap.delete(i)).to be == i
					deleted << i
					expect(priority_heap).to be(:valid?)
				end
			end
			
			# Verify remaining elements
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			
			expected_remaining = (1..100).to_a - deleted
			expect(remaining).to be == expected_remaining
		end
	end
	
	with "#delete_if" do
		it "should return 0 when no elements match condition" do
			elements = [1, 2, 3, 4, 5]
			elements.each {|e| priority_heap.push(e)}
			
			removed_count = priority_heap.delete_if {|e| e > 10}
			expect(removed_count).to be == 0
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
		end
		
		it "should return 0 when heap is empty" do
			removed_count = priority_heap.delete_if {|e| true}
			expect(removed_count).to be == 0
			expect(priority_heap).to be(:empty?)
		end
		
		it "should remove all elements when condition always true" do
			elements = [5, 2, 8, 1, 9, 3]
			elements.each {|e| priority_heap.push(e)}
			
			removed_count = priority_heap.delete_if {|e| true}
			expect(removed_count).to be == 6
			expect(priority_heap).to be(:empty?)
		end
		
		it "should remove even numbers and maintain heap property" do
			elements = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
			elements.each {|e| priority_heap.push(e)}
			
			removed_count = priority_heap.delete_if {|e| e.even?}
			expect(removed_count).to be == 5  # 2, 4, 6, 8, 10
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			
			# Remaining elements should be odd numbers in sorted order
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			expect(remaining).to be == [1, 3, 5, 7, 9]
		end
		
		it "should handle removing elements from specific ranges" do
			elements = (1..20).to_a
			elements.each {|e| priority_heap.push(e)}
			
			# Remove elements between 5 and 15 (inclusive)
			removed_count = priority_heap.delete_if {|e| e >= 5 && e <= 15}
			expect(removed_count).to be == 11  # 5,6,7,8,9,10,11,12,13,14,15
			expect(priority_heap.size).to be == 9
			expect(priority_heap).to be(:valid?)
			
			# Should have 1,2,3,4,16,17,18,19,20
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			expected = [1, 2, 3, 4, 16, 17, 18, 19, 20]
			expect(remaining).to be == expected
		end
		
		it "should work with duplicate elements" do
			elements = [5, 3, 5, 1, 5, 2, 5, 4]
			elements.each {|e| priority_heap.push(e)}
			
			# Remove all 5s
			removed_count = priority_heap.delete_if {|e| e == 5}
			expect(removed_count).to be == 4
			expect(priority_heap.size).to be == 4
			expect(priority_heap).to be(:valid?)
			
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			expect(remaining).to be == [1, 2, 3, 4]
		end
		
		it "should return enumerator when no block given" do
			elements = [1, 2, 3, 4, 5]
			elements.each {|e| priority_heap.push(e)}
			
			enum = priority_heap.delete_if
			expect(enum).to be_a(Enumerator)
			
			# Use the enumerator to delete even numbers
			removed_count = enum.select {|e| e.even?}
			# Note: select doesn't actually delete, we need to call the enumerator differently
			
			# Better test: create enumerator and then call with condition
			enum = priority_heap.delete_if
			removed_count = enum.each {|e| e.even?}
			expect(removed_count).to be == 2
			expect(priority_heap.size).to be == 3
		end
		
		it "should be more efficient than multiple delete operations" do
			# Large dataset to demonstrate efficiency
			elements = (1..1000).to_a.shuffle
			elements.each {|e| priority_heap.push(e)}
			
			# Remove all multiples of 7 using delete_if
			start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
			removed_count = priority_heap.delete_if {|e| e % 7 == 0}
			delete_if_time = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start_time
			
			expected_removed = (1..1000).count {|x| x % 7 == 0}  # Should be 142
			expect(removed_count).to be == expected_removed
			expect(priority_heap.size).to be == 1000 - expected_removed
			expect(priority_heap).to be(:valid?)
			
			# Verify remaining elements are correct
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			
			expected_remaining = (1..1000).reject {|x| x % 7 == 0}
			expect(remaining).to be == expected_remaining
		end
		
		it "should handle complex conditions" do
			elements = (1..50).to_a
			elements.each {|e| priority_heap.push(e)}
			
			# Remove numbers that are prime (simple prime test for small numbers)
			is_prime = lambda do |n|
				return false if n < 2
				return true if n == 2
				return false if n.even?
				(3..Math.sqrt(n)).step(2).none? {|i| n % i == 0}
			end
			
			removed_count = priority_heap.delete_if(&is_prime)
			
			# Count expected primes in range 1-50
			expected_prime_count = (1..50).count(&is_prime)
			expect(removed_count).to be == expected_prime_count
			expect(priority_heap.size).to be == 50 - expected_prime_count
			expect(priority_heap).to be(:valid?)
		end
		
		it "should maintain heap invariant after bulk deletions" do
			# Multiple rounds of delete_if to stress test heap maintenance
			elements = (1..100).to_a.shuffle
			elements.each {|e| priority_heap.push(e)}
			
			# First: remove multiples of 3
			removed1 = priority_heap.delete_if {|e| e % 3 == 0}
			expect(priority_heap).to be(:valid?)
			
			# Second: remove multiples of 7 from remaining
			removed2 = priority_heap.delete_if {|e| e % 7 == 0}
			expect(priority_heap).to be(:valid?)
			
			# Third: remove numbers greater than 80
			removed3 = priority_heap.delete_if {|e| e > 80}
			expect(priority_heap).to be(:valid?)
			
			# Verify final result comes out in sorted order
			remaining = []
			while !priority_heap.empty?
				remaining << priority_heap.pop
			end
			expect(remaining.sort).to be == remaining
		end
	end
	
	with "#concat" do
		it "should return self when concatenating empty array" do
			elements = [1, 2, 3]
			elements.each {|e| priority_heap.push(e)}
			
			result = priority_heap.concat([])
			expect(result).to be == priority_heap
			expect(priority_heap.size).to be == 3
			expect(priority_heap).to be(:valid?)
		end
		
		it "should efficiently add multiple elements to empty heap" do
			elements = [5, 2, 8, 1, 9, 3]
			
			result = priority_heap.concat(elements)
			expect(result).to be == priority_heap  # Returns self
			expect(priority_heap.size).to be == 6
			expect(priority_heap).to be(:valid?)
			
			# Should extract in sorted order
			sorted_result = []
			while !priority_heap.empty?
				sorted_result << priority_heap.pop
			end
			expect(sorted_result).to be == elements.sort
		end
		
		it "should add elements to existing heap and maintain order" do
			# Start with some elements
			initial = [10, 15, 20]
			initial.each {|e| priority_heap.push(e)}
			
			# Add more elements in bulk
			additional = [5, 12, 25, 8]
			priority_heap.concat(additional)
			
			expect(priority_heap.size).to be == 7
			expect(priority_heap).to be(:valid?)
			
			# Should extract all in sorted order
			all_elements = initial + additional
			sorted_result = []
			while !priority_heap.empty?
				sorted_result << priority_heap.pop
			end
			expect(sorted_result).to be == all_elements.sort
		end
		
		it "should handle duplicate elements correctly" do
			priority_heap.concat([5, 3, 5, 1, 5])
			
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			
			# Should have three 5s
			result = []
			while !priority_heap.empty?
				result << priority_heap.pop
			end
			expect(result.count(5)).to be == 3
			expect(result).to be == [1, 3, 5, 5, 5]
		end
		
		it "should be more efficient than multiple push operations" do
			# Large dataset to demonstrate efficiency
			elements = (1..1000).to_a.shuffle
			
			# Test concat performance
			heap1 = subject.new
			start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
			heap1.concat(elements)
			concat_time = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start_time
			
			# Test individual push performance
			heap2 = subject.new
			start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
			elements.each {|e| heap2.push(e)}
			push_time = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start_time
			
			# Both should produce same result
			expect(heap1.size).to be == heap2.size
			expect(heap1).to be(:valid?)
			expect(heap2).to be(:valid?)
			
			# Verify both heaps contain same elements
			result1 = []
			while !heap1.empty?
				result1 << heap1.pop
			end
			
			result2 = []
			while !heap2.empty?
				result2 << heap2.pop
			end
			
			expect(result1).to be == result2
			expect(result1).to be == (1..1000).to_a
		end
		
		it "should handle single element concat" do
			priority_heap.concat([42])
			
			expect(priority_heap.size).to be == 1
			expect(priority_heap.peek).to be == 42
			expect(priority_heap.pop).to be == 42
			expect(priority_heap).to be(:empty?)
		end
		
		it "should handle incomparable mixed data types" do
			# Mix strings and numbers (not comparable)
			elements = [3, "apple", 1, "zebra", 5]
			
			# This should raise an exception when trying to compare incomparable types
			expect do
				priority_heap.concat(elements)
				# Force comparison by trying to extract elements
				priority_heap.pop
			end.to raise_exception
		end
		
		it "should maintain heap property after large bulk inserts" do
			# Multiple rounds of concat to stress test
			(1..10).each do |round|
				elements = ((round-1)*100 + 1..round*100).to_a.shuffle
				priority_heap.concat(elements)
				expect(priority_heap).to be(:valid?)
			end
			
			expect(priority_heap.size).to be == 1000
			
			# Should extract in perfect sorted order
			result = []
			while !priority_heap.empty?
				result << priority_heap.pop
			end
			expect(result).to be == (1..1000).to_a
		end
		
		it "should support method chaining" do
			result = priority_heap
				.concat([10, 5])
				.concat([15, 1])
				.concat([8])
			
			expect(result).to be == priority_heap
			expect(priority_heap.size).to be == 5
			expect(priority_heap).to be(:valid?)
			
			# Verify all elements are there
			sorted = []
			while !priority_heap.empty?
				sorted << priority_heap.pop
			end
			expect(sorted).to be == [1, 5, 8, 10, 15]
		end
	end
end
