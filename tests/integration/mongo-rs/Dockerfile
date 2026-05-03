FROM mongo:7

COPY mongo-rs-entrypoint.sh /usr/local/bin/mongo-rs-entrypoint.sh
RUN chmod +x /usr/local/bin/mongo-rs-entrypoint.sh

ENTRYPOINT ["/usr/local/bin/mongo-rs-entrypoint.sh"]
