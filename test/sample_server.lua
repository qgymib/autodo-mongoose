local mongoose = require("mongoose")

local server_opts = {
    serve_dir = "/home/qgymib/workspace/autodo-mongoose/test/",
    listen_url = "http://127.0.0.1:5001"
}
local server = mongoose.http_server(server_opts)
assert(server:run() == true)

io.write("server listen on " .. server_opts.listen_url .. "\n")
auto.sleep(100000000)
