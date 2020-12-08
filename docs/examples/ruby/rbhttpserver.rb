#!/usr/bin/env ruby

require 'webrick'

server = WEBrick::HTTPServer.new(
    :Port => 8088,
)

server.mount_proc '/' do |req, res|
  res.body = 'Example Domain Cleartext'
end

server.start




# After that, make the script an executable.
#
# To start the server, run ./script/http_server.rb

# Start HTTP server
#./script/http_server.rb
# => [2015-08-02 07:19:17] INFO  WEBrick 1.3.1
# => [2015-08-02 07:19:17] INFO  ruby 2.2.2 (2015-04-13) [x86_64-linux]
# => [2015-08-02 07:19:17] INFO  WEBrick::HTTPServer#start: pid=15600 port=8000
# To check if the server is running correctly, invoke curl – you may need to install this program on your machine.

# curl http://localhost:8088
# => Example Domain Cleartext
