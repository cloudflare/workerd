FROM node:22-alpine

WORKDIR /usr/src/app
RUN echo '{"name": "simple-node-app", "version": "1.0.0", "dependencies": {"ws": "^8.0.0"}}' > package.json
RUN npm install

COPY simple-node-app.js app.js
EXPOSE 8080

# Expose another port so we can test not listening here
EXPOSE 8081

CMD [ "node", "app.js" ]
