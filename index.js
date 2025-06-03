const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 3000;
const welcomePath = path.join(__dirname, 'welcome.html');

const server = http.createServer((req, res) => {
    fs.readFile(welcomePath, (err, data) => {
        if (err) {
            res.writeHead(404, { 'Content-Type': 'text/plain' });
            res.end('welcome.html not found');
            return;
        }
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(data);
    });
});

server.listen(PORT, () => {
    console.log(`Server running at http://localhost:${PORT}/`);
});