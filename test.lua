local qpack = require 'qpack.safe'
local cjson = require 'cjson.safe'
local basexx = require 'basexx'

local str = '{"SAMPLE.w00000.flag.string":[[1637979480080,"N"],[1637979485080,"N"]],"SAMPLE.w00000.value.float":[[1637979480080,29.1100001],[1637979485080,29.1100001]],"SAMPLE.w00000.cou.float":[[1637979480080,206.09879787908],[1637979485080,145.5500001]]}'

local t = cjson.decode(str)
print(cjson.encode(t))

local str2 = qpack.encode(t)
print(basexx.to_hex(str2))

local t2, err = qpack.decode(str2)
if not t2 then
	print(err)
end

print(cjson.encode(t2))
