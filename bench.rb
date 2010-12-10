#!/opt/local/bin/ruby19

require 'msgpack'
require 'json'
require 'benchmark'

data_template = {'abcdef' => 1, 'qqq' => 13, '19' => [1, 2, 3, 4]};
data = [];

n = 50000
#for (var i = 0; i < 500000; i++) {
n.times do
    data.push(Marshal.load(Marshal.dump(data_template)));
end


mpBuf = nil
jsonStr = nil
marshalStr = nil
Benchmark.bm do |x|
    x.report ('msgpack pack  ') { data.each {|d| mpBuf = MessagePack.pack(d) }}
    x.report ('msgpack unpack') { data.each {|d| MessagePack.pack(mpBuf) }}
    x.report ('json pack     ') { data.each {|d| jsonStr = d.to_json }}
    x.report ('json unpack   ') { data.each {|d| JSON.parse(jsonStr) }}
    x.report ('marshal pack  ') { data.each {|d| marshalStr = Marshal.dump(d) }}
    x.report ('marshal unpack') { data.each {|d| Marshal.load(marshalStr) }}
end

p mpBuf
puts MessagePack.pack(mpBuf)
puts jsonStr
puts JSON.parse(jsonStr)
p marshalStr
puts Marshal.load(marshalStr)
