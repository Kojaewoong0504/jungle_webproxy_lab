math.randomseed(os.time())

request = function()
  local i = math.random(1, 10)
  local uri = "/http://localhost:26814/home" .. i .. ".html"
  return wrk.format("GET", uri, {["Host"] = "localhost:26814"})
end
