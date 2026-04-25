(function () {
  const replicaSetName = "scribe-rs";
  const memberHost = "localhost:27017";
  const deadline = Date.now() + 60000;

  try {
    rs.status();
  } catch (e) {
    rs.initiate({
      _id: replicaSetName,
      members: [{ _id: 0, host: memberHost }],
    });
  }

  while (Date.now() < deadline) {
    try {
      const hello = db.adminCommand({ hello: 1 });
      if (hello.isWritablePrimary === true || hello.ismaster === true) {
        break;
      }
    } catch (e) {
      // Keep waiting while the local single-node replica set elects itself.
    }
    sleep(500);
  }

  const finalHello = db.adminCommand({ hello: 1 });
  if (finalHello.isWritablePrimary !== true && finalHello.ismaster !== true) {
    throw new Error("MongoDB replica set did not become PRIMARY");
  }

  const scribeDb = db.getSiblingDB("scribe_test");
  if (!scribeDb.getCollectionNames().includes("users")) {
    scribeDb.createCollection("users");
  }
  scribeDb.runCommand({
    collMod: "users",
    changeStreamPreAndPostImages: { enabled: true },
  });
  scribeDb.users.updateOne(
    { _id: "seed" },
    { $setOnInsert: { role: "seed" } },
    { upsert: true }
  );
})();
