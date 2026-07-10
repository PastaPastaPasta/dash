//! Generates GroveDB proof test vectors for the C++ verifier in
//! src/platform/proof/ (tested by src/test/platform_proof_tests.cpp).
//!
//! Everything here is deterministic: fixed keys and values, no randomness,
//! no timestamps. Output JSON files are written to src/test/data/platform/.
//!
//! Usage (from the repo root):
//!   cd contrib/devtools/platform-test-vectors
//!   cargo run --release -- ../../../src/test/data/platform

use std::collections::BTreeMap;
use std::path::Path;

use grovedb::operations::proof::{GroveDBProof, LayerProof, MerkOnlyLayerProof, ProofBytes};
use grovedb::reference_path::ReferencePathType;
use grovedb::{Element, GroveDb, PathQuery, Query, QueryItem, SizedQuery};
use grovedb_merk::proofs::query::QueryProofVerify;
use grovedb_version::version::GroveVersion;
use serde_json::{json, Value};

fn hex_of(b: &[u8]) -> String {
    hex::encode(b)
}

/// JSON description of a QueryItem, mirrored by the C++ test loader.
fn query_item_json(item: &QueryItem) -> Value {
    match item {
        QueryItem::Key(k) => json!({"type": "key", "key": hex_of(k)}),
        QueryItem::Range(r) => {
            json!({"type": "range", "start": hex_of(&r.start), "end": hex_of(&r.end)})
        }
        QueryItem::RangeInclusive(r) => {
            json!({"type": "range_inclusive", "start": hex_of(r.start()), "end": hex_of(r.end())})
        }
        QueryItem::RangeFull(_) => json!({"type": "range_full"}),
        other => panic!("unsupported query item in vector generator: {:?}", other),
    }
}

/// JSON description of a Query (items + default subquery branch), mirrored by
/// the C++ test loader. Conditional subquery branches are not used by these
/// vectors.
fn query_json(q: &Query) -> Value {
    assert!(
        q.conditional_subquery_branches.is_none(),
        "vector generator does not describe conditional subquery branches"
    );
    let items: Vec<Value> = q.items.iter().map(query_item_json).collect();
    let subquery_path: Value = match &q.default_subquery_branch.subquery_path {
        Some(p) => Value::Array(p.iter().map(|s| Value::String(hex_of(s))).collect()),
        None => Value::Null,
    };
    let subquery: Value = match &q.default_subquery_branch.subquery {
        Some(sq) => query_json(sq),
        None => Value::Null,
    };
    json!({
        "items": items,
        "left_to_right": q.left_to_right,
        "default_subquery_path": subquery_path,
        "default_subquery": subquery,
    })
}

fn path_query_json(pq: &PathQuery) -> Value {
    let path: Vec<Value> = pq.path.iter().map(|s| Value::String(hex_of(s))).collect();
    json!({
        "path": path,
        "query": query_json(&pq.query.query),
        "limit": pq.query.limit,
    })
}

/// Build the fixture database. All contents are deterministic.
fn build_db(db: &GroveDb, v: &GroveVersion) {
    let insert = |path: &[&[u8]], key: &[u8], element: Element| {
        db.insert(path, key, element, None, None, v)
            .unwrap()
            .expect("insert failed");
    };

    // Top-level trees
    insert(&[], b"items", Element::empty_tree());
    insert(&[], b"index", Element::empty_tree());
    insert(&[], b"balances", Element::new_sum_tree(None));
    insert(&[], b"docs", Element::empty_tree());
    insert(&[], b"empty", Element::empty_tree());

    // items: plain items i1..i5, i3 carries flags
    insert(&[b"items"], b"i1", Element::new_item(b"value one".to_vec()));
    insert(&[b"items"], b"i2", Element::new_item(b"value two".to_vec()));
    insert(
        &[b"items"],
        b"i3",
        Element::new_item_with_flags(b"value three".to_vec(), Some(vec![0x0d, 0x0e])),
    );
    insert(&[b"items"], b"i4", Element::new_item(b"value four".to_vec()));
    insert(&[b"items"], b"i5", Element::new_item(b"value five".to_vec()));

    // index: references into items
    insert(
        &[b"index"],
        b"r1",
        Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            b"items".to_vec(),
            b"i1".to_vec(),
        ])),
    );
    insert(
        &[b"index"],
        b"r2",
        Element::new_reference(ReferencePathType::UpstreamRootHeightReference(
            0,
            vec![b"items".to_vec(), b"i2".to_vec()],
        )),
    );

    // balances: a sum tree with sum items
    insert(&[b"balances"], b"alice", Element::new_sum_item(1000));
    insert(&[b"balances"], b"bob", Element::new_sum_item(-250));
    insert(&[b"balances"], b"carol", Element::new_sum_item(42));

    // docs: two user subtrees with documents
    insert(&[b"docs"], b"user1", Element::empty_tree());
    insert(&[b"docs"], b"user2", Element::empty_tree());
    insert(
        &[b"docs", b"user1"],
        b"d1",
        Element::new_item(b"doc one".to_vec()),
    );
    insert(
        &[b"docs", b"user1"],
        b"d2",
        Element::new_item(b"doc two".to_vec()),
    );
    insert(
        &[b"docs", b"user2"],
        b"d3",
        Element::new_item(b"doc three".to_vec()),
    );
}

// ---------------------------------------------------------------------------
// element vectors
// ---------------------------------------------------------------------------

fn element_vectors(v: &GroveVersion) -> Value {
    let root_key: Vec<u8> = (0u8..32).collect();
    let cases: Vec<(&str, Element)> = vec![
        ("item_no_flags", Element::new_item(b"hello".to_vec())),
        ("item_empty_value", Element::new_item(vec![])),
        (
            "item_with_flags",
            Element::new_item_with_flags(vec![1, 2, 3], Some(vec![7, 8])),
        ),
        (
            "item_large_value",
            // 300-byte value: exercises the >=251 bincode varint length form
            // (0xfb marker + big-endian u16).
            Element::new_item(vec![0xab; 300]),
        ),
        ("sum_item_positive", Element::new_sum_item(1000)),
        ("sum_item_negative", Element::new_sum_item(-42)),
        (
            "sum_item_with_flags",
            Element::new_sum_item_with_flags(5, Some(vec![1])),
        ),
        ("tree_empty_root_key", Element::empty_tree()),
        ("tree_nonempty", Element::new_tree(Some(root_key.clone()))),
        (
            "tree_with_flags",
            Element::new_tree_with_flags(None, Some(vec![5])),
        ),
        ("sum_tree_empty", Element::new_sum_tree(None)),
        (
            "sum_tree_nonempty",
            Element::new_sum_tree_with_flags_and_sum_value(Some(root_key.clone()), 123, None),
        ),
        (
            "reference_absolute",
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                vec![0],
                b"abcd".to_vec(),
                vec![5],
            ])),
        ),
        (
            "reference_absolute_hops_flags",
            Element::new_reference_with_max_hops_and_flags(
                ReferencePathType::AbsolutePathReference(vec![b"items".to_vec(), b"i1".to_vec()]),
                Some(3),
                Some(vec![9]),
            ),
        ),
        (
            "reference_upstream_root_height",
            Element::new_reference(ReferencePathType::UpstreamRootHeightReference(
                1,
                vec![b"p".to_vec(), b"q".to_vec()],
            )),
        ),
        (
            "reference_upstream_root_height_parent_addition",
            Element::new_reference(
                ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                    2,
                    vec![b"x".to_vec(), b"y".to_vec()],
                ),
            ),
        ),
        (
            "reference_upstream_from_element_height",
            Element::new_reference(ReferencePathType::UpstreamFromElementHeightReference(
                1,
                vec![b"p".to_vec(), b"q".to_vec()],
            )),
        ),
        (
            "reference_cousin",
            Element::new_reference(ReferencePathType::CousinReference(b"c".to_vec())),
        ),
        (
            "reference_removed_cousin",
            Element::new_reference(ReferencePathType::RemovedCousinReference(vec![
                b"m".to_vec(),
                b"n".to_vec(),
            ])),
        ),
        (
            "reference_sibling",
            Element::new_reference(ReferencePathType::SiblingReference(b"s".to_vec())),
        ),
    ];

    let vectors: Vec<Value> = cases
        .into_iter()
        .map(|(name, element)| {
            let serialized = element.serialize(v).expect("serialize element");
            json!({
                "name": name,
                "description": format!("{}", element),
                "serialized_hex": hex_of(&serialized),
            })
        })
        .collect();
    json!({ "vectors": vectors })
}

// ---------------------------------------------------------------------------
// merk proof vectors (single-subtree proofs, extracted from GroveDB proofs)
// ---------------------------------------------------------------------------

const BINCODE_CONFIG: bincode::config::Configuration<
    bincode::config::BigEndian,
    bincode::config::Varint,
    bincode::config::NoLimit,
> = bincode::config::standard().with_big_endian().with_no_limit();

fn decode_envelope(proof: &[u8]) -> GroveDBProof {
    let (decoded, consumed): (GroveDBProof, usize) =
        bincode::decode_from_slice(proof, BINCODE_CONFIG).expect("decode envelope");
    assert_eq!(consumed, proof.len(), "trailing envelope bytes");
    decoded
}

/// Walk the envelope down the given path and return the merk proof bytes for
/// that layer.
fn merk_proof_at_path(proof: &GroveDBProof, path: &[&[u8]]) -> Vec<u8> {
    fn walk_v0<'a>(layer: &'a MerkOnlyLayerProof, path: &[&[u8]]) -> &'a Vec<u8> {
        match path.split_first() {
            None => &layer.merk_proof,
            Some((seg, rest)) => walk_v0(
                layer
                    .lower_layers
                    .get(*seg)
                    .expect("path segment missing in v0 proof"),
                rest,
            ),
        }
    }
    fn walk_v1<'a>(layer: &'a LayerProof, path: &[&[u8]]) -> &'a Vec<u8> {
        match path.split_first() {
            None => match &layer.merk_proof {
                ProofBytes::Merk(bytes) => bytes,
                _ => panic!("expected merk proof bytes"),
            },
            Some((seg, rest)) => walk_v1(
                layer
                    .lower_layers
                    .get(*seg)
                    .expect("path segment missing in v1 proof"),
                rest,
            ),
        }
    }
    match proof {
        GroveDBProof::V0(p) => walk_v0(&p.root_layer, path).clone(),
        GroveDBProof::V1(p) => walk_v1(&p.root_layer, path).clone(),
    }
}

struct MerkCase<'a> {
    name: &'a str,
    path: Vec<&'a [u8]>,
    items: Vec<QueryItem>,
    limit: Option<u16>,
    /// GroveVersion used for proving; GROVE_V1/GROVE_V2 produce V0 envelopes
    /// (lenient merk proof version 0), GROVE_V3 produces V1 (strict version 1).
    grove_version: &'a GroveVersion,
    proof_version: u16,
}

fn merk_vectors(db: &GroveDb, latest: &GroveVersion, first: &GroveVersion) -> Value {
    let cases = vec![
        MerkCase {
            name: "items_single_key_present",
            path: vec![b"items"],
            items: vec![QueryItem::Key(b"i2".to_vec())],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_single_key_absent",
            path: vec![b"items"],
            items: vec![QueryItem::Key(b"i9".to_vec())],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_multiple_keys",
            path: vec![b"items"],
            items: vec![
                QueryItem::Key(b"i1".to_vec()),
                QueryItem::Key(b"i3".to_vec()),
                QueryItem::Key(b"i5".to_vec()),
            ],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_range",
            path: vec![b"items"],
            items: vec![QueryItem::Range(b"i2".to_vec()..b"i4".to_vec())],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_range_inclusive",
            path: vec![b"items"],
            items: vec![QueryItem::RangeInclusive(b"i2".to_vec()..=b"i4".to_vec())],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_range_full_limit_2",
            path: vec![b"items"],
            items: vec![QueryItem::RangeFull(std::ops::RangeFull)],
            limit: Some(2),
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "sum_tree_keys",
            path: vec![b"balances"],
            items: vec![
                QueryItem::Key(b"alice".to_vec()),
                QueryItem::Key(b"bob".to_vec()),
            ],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "sum_tree_range_full",
            path: vec![b"balances"],
            items: vec![QueryItem::RangeFull(std::ops::RangeFull)],
            limit: None,
            grove_version: latest,
            proof_version: 1,
        },
        MerkCase {
            name: "items_multiple_keys_v0",
            path: vec![b"items"],
            items: vec![
                QueryItem::Key(b"i1".to_vec()),
                QueryItem::Key(b"i3".to_vec()),
            ],
            limit: None,
            grove_version: first,
            proof_version: 0,
        },
    ];

    let mut vectors = Vec::new();
    for case in cases {
        let mut query = Query::new();
        for item in case.items.clone() {
            query.insert_item(item);
        }
        let path_query = PathQuery::new(
            case.path.iter().map(|p| p.to_vec()).collect(),
            SizedQuery::new(query.clone(), case.limit, None),
        );
        let proof_bytes = db
            .prove_query(&path_query, None, case.grove_version)
            .unwrap()
            .expect("prove");
        // Sanity: the full proof verifies against the db root hash.
        let root_hash = db.root_hash(None, case.grove_version).unwrap().unwrap();
        let (verified_root, _) =
            GroveDb::verify_query_raw(&proof_bytes, &path_query, case.grove_version)
                .expect("full proof verifies");
        assert_eq!(verified_root, root_hash, "{}", case.name);

        // Extract the single-subtree merk proof and verify it standalone to
        // obtain the expected merk root hash and result set.
        let envelope = decode_envelope(&proof_bytes);
        let merk_bytes = merk_proof_at_path(&envelope, &case.path);
        let (merk_root, merk_result) = query
            .execute_proof(&merk_bytes, case.limit, true, case.proof_version)
            .unwrap()
            .expect("merk proof verifies");

        let results: Vec<Value> = merk_result
            .result_set
            .iter()
            .map(|r| {
                json!({
                    "key_hex": hex_of(&r.key),
                    "value_hex": r.value.as_ref().map(|v| hex_of(v)),
                })
            })
            .collect();

        let items: Vec<Value> = case.items.iter().map(query_item_json).collect();
        vectors.push(json!({
            "name": case.name,
            "query": {
                "items": items,
                "limit": case.limit,
                "left_to_right": true,
            },
            "proof_version": case.proof_version,
            "proof_hex": hex_of(&merk_bytes),
            "expected_root_hash_hex": hex_of(&merk_root),
            "expected_results": results,
        }));
    }
    json!({ "vectors": vectors })
}

// ---------------------------------------------------------------------------
// grovedb (layered) proof vectors
// ---------------------------------------------------------------------------

struct GroveCase<'a> {
    name: &'a str,
    path_query: PathQuery,
    grove_version: &'a GroveVersion,
}

fn grove_vectors(db: &GroveDb, latest: &GroveVersion, first: &GroveVersion) -> Value {
    let key_query = |path: Vec<Vec<u8>>, keys: Vec<&[u8]>, limit: Option<u16>| -> PathQuery {
        let mut query = Query::new();
        for key in keys {
            query.insert_item(QueryItem::Key(key.to_vec()));
        }
        PathQuery::new(path, SizedQuery::new(query, limit, None))
    };

    // Three-level query using a default subquery:
    // path ["docs"], items [Key("user1")], subquery: all items.
    let subquery_case = {
        let mut subquery = Query::new();
        subquery.insert_item(QueryItem::RangeFull(std::ops::RangeFull));
        let mut query = Query::new();
        query.insert_item(QueryItem::Key(b"user1".to_vec()));
        query.set_subquery(subquery);
        PathQuery::new(vec![b"docs".to_vec()], SizedQuery::new(query, None, None))
    };

    // Sum tree range query
    let sum_tree_case = {
        let mut query = Query::new();
        query.insert_item(QueryItem::RangeFull(std::ops::RangeFull));
        PathQuery::new(
            vec![b"balances".to_vec()],
            SizedQuery::new(query, None, None),
        )
    };

    let cases = vec![
        GroveCase {
            name: "two_level_items_v1",
            path_query: key_query(vec![b"items".to_vec()], vec![b"i1", b"i3"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "two_level_items_v0",
            path_query: key_query(vec![b"items".to_vec()], vec![b"i1", b"i3"], None),
            grove_version: first,
        },
        GroveCase {
            name: "absence_v1",
            path_query: key_query(vec![b"items".to_vec()], vec![b"i9"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "absence_between_keys_v1",
            path_query: key_query(vec![b"items".to_vec()], vec![b"i2", b"i25", b"i3"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "reference_absolute_v1",
            path_query: key_query(vec![b"index".to_vec()], vec![b"r1"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "reference_upstream_v1",
            path_query: key_query(vec![b"index".to_vec()], vec![b"r2"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "sum_tree_v1",
            path_query: sum_tree_case,
            grove_version: latest,
        },
        GroveCase {
            name: "three_level_path_v1",
            path_query: key_query(
                vec![b"docs".to_vec(), b"user1".to_vec()],
                vec![b"d1"],
                None,
            ),
            grove_version: latest,
        },
        GroveCase {
            name: "three_level_subquery_v1",
            path_query: subquery_case,
            grove_version: latest,
        },
        GroveCase {
            name: "root_empty_tree_v1",
            path_query: key_query(vec![], vec![b"empty"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "root_tree_no_subquery_v1",
            path_query: key_query(vec![], vec![b"items"], None),
            grove_version: latest,
        },
        GroveCase {
            name: "root_tree_no_subquery_v0",
            path_query: key_query(vec![], vec![b"items"], None),
            grove_version: first,
        },
    ];

    let mut vectors = Vec::new();
    for case in cases {
        let proof_bytes = db
            .prove_query(&case.path_query, None, case.grove_version)
            .unwrap()
            .expect("prove");
        let root_hash = db.root_hash(None, case.grove_version).unwrap().unwrap();
        let (verified_root, results) =
            GroveDb::verify_query_raw(&proof_bytes, &case.path_query, case.grove_version)
                .expect("proof verifies raw");
        assert_eq!(verified_root, root_hash, "{}", case.name);

        // Cross-check with the deserializing verifier as well (skips
        // element deserialization errors would surface here).
        let (verified_root2, _) = GroveDb::verify_query_with_options(
            &proof_bytes,
            &case.path_query,
            grovedb::VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: true,
            },
            case.grove_version,
        )
        .expect("proof verifies deserialized");
        assert_eq!(verified_root2, root_hash, "{}", case.name);

        let results_json: Vec<Value> = results
            .iter()
            .map(|r| {
                json!({
                    "path": r.path.iter().map(|s| Value::String(hex_of(s))).collect::<Vec<_>>(),
                    "key_hex": hex_of(&r.key),
                    "element_hex": hex_of(&r.value),
                })
            })
            .collect();

        vectors.push(json!({
            "name": case.name,
            "path_query": path_query_json(&case.path_query),
            "proof_hex": hex_of(&proof_bytes),
            "expected_root_hash_hex": hex_of(&root_hash),
            "expected_results": results_json,
        }));
    }
    json!({ "vectors": vectors })
}

fn main() {
    let out_dir = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "../../../src/test/data/platform".to_string());
    let out_dir = Path::new(&out_dir);
    std::fs::create_dir_all(out_dir).expect("create output dir");

    let latest = GroveVersion::latest();
    let first = &grovedb_version::version::GROVE_VERSIONS[0];

    let tmp = tempfile::tempdir().expect("tempdir");
    let db = GroveDb::open(tmp.path()).expect("open grovedb");
    build_db(&db, latest);

    let write = |name: &str, value: Value| {
        let path = out_dir.join(name);
        let mut text = serde_json::to_string_pretty(&value).expect("serialize json");
        text.push('\n');
        std::fs::write(&path, text).expect("write json");
        println!("wrote {}", path.display());
    };

    write("element_vectors.json", element_vectors(latest));
    write("merk_proof_vectors.json", merk_vectors(&db, latest, first));
    write(
        "grovedb_proof_vectors.json",
        grove_vectors(&db, latest, first),
    );

    // Keep BTreeMap import used in both envelope variants.
    let _: Option<BTreeMap<Vec<u8>, ()>> = None;
}
