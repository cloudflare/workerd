FROM node:22-alpine

WORKDIR /usr/src/app
RUN echo '{"name": "simple-node-app", "version": "1.0.0", "dependencies": {"ws": "^8.0.0"}}' > package.json
RUN npm install

COPY simple-node-app.js app.js
EXPOSE 8080
CMD [ "node", "app.js" ]
