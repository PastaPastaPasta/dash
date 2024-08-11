import os
import json
import requests
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives import hashes

def get_public_key(token, repo):
    url = f"https://api.github.com/repos/{repo}/actions/secrets/public-key"
    headers = {"Authorization": f"token {token}"}
    response = requests.get(url, headers=headers)
    return response.json()

def encrypt_secret(public_key: dict, secret_value: str):
    key = serialization.load_pem_public_key(
        bytes(public_key['key'], 'utf-8'), backend=default_backend()
    )
    encrypted = key.encrypt(
        bytes(secret_value, 'utf-8'),
        padding.OAEP(
            mgf=padding.MGF1(algorithm=hashes.SHA256()),
            algorithm=hashes.SHA256(),
            label=None
        )
    )
    return encrypted

def update_secret(token, repo, secret_name, encrypted_value, key_id):
    url = f"https://api.github.com/repos/{repo}/actions/secrets/{secret_name}"
    headers = {"Authorization": f"token {token}"}
    data = {
        "encrypted_value": encrypted_value,
        "key_id": key_id
    }
    response = requests.put(url, headers=headers, data=json.dumps(data))
    return response.status_code

def main():
    token = os.getenv('ACCESS_TOKEN')
    repo_name = os.getenv('REPO_NAME')
    secret_name = os.getenv('SECRET_NAME')
    secret_value = os.getenv('SECRET_VALUE')

    public_key = get_public_key(token, repo_name)
    encrypted_value = encrypt_secret(public_key, secret_value)
    result = update_secret(token, repo_name, secret_name, encrypted_value, public_key['key_id'])
    print(f"Secret update response code: {result}")

if __name__ == "__main__":
    main()