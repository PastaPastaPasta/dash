import os
import json
import requests
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives import hashes

def get_public_key(token, owner, repo):
    url = f"https://api.github.com/repos/{repo}/actions/secrets/public-key"
    print(url)
    headers = {"Accept": "application/vnd.github+json",
               "Authorization": f"Bearer {token}",
               "X-GitHub-Api-Version": "2022-11-28"}
    response = requests.get(url, headers=headers)
    public_key = response.json()
    print("Public Key:", public_key)  # Add this to debug
    return public_key

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
    headers = {"Accept": "application/vnd.github+json",
               "Authorization": f"Bearer {token}",
               "X-GitHub-Api-Version": "2022-11-28"}
    data = {
        "encrypted_value": encrypted_value,
        "key_id": key_id
    }
    response = requests.put(url, headers=headers, data=json.dumps(data))
    return response.status_code

def main():
    token = os.getenv('PAT')
    owner = os.getenv('OWNER_NAME')
    repo_name = os.getenv('REPO_NAME')
    secret_name = os.getenv('SECRET_NAME')
    secret_value = os.getenv('SECRET_VALUE')

    public_key = get_public_key(token, owner, repo_name)
    encrypted_value = encrypt_secret(public_key, secret_value)
    result = update_secret(token, repo_name, secret_name, encrypted_value, public_key['key_id'])
    print(f"Secret update response code: {result}")

if __name__ == "__main__":
    main()