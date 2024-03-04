# An HTTP module designed for lanyt.js

## Introduction

This module is imported through a dynamic link library from [lanyt.js](https://github.com/lanytcc/lanyt.js), providing simple yet comprehensive HTTP send and receive functions.

## Installation

```shell
git clone https://github.com/lanytcc/quickjs.git
git clone https://github.com/lanytcc/http.git
cd http
xmake b http
xmake install -o /usr/local http
```

## How to use

```javascript
import { http } from "http.so"; // if you use windows, you need use "http.dll"

const server = new http.server();

server.listen("0.0.0.0", 8080);
server.on("/", (req) => {
    var r = req.get();
    console.log(r.method);
    return new http.response({
        status: 200,
        body: "Hello, world!"
    });
});
server.dispatch();
```
